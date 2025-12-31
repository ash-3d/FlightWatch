/*
Purpose: Firmware entry point for ESP32.
Responsibilities:
- Initialize serial, connect to Wi‑Fi, and construct fetchers and display.
- Periodically fetch state vectors (OpenSky), enrich flights (AeroAPI), and render.
Configuration: UserConfiguration (location/filters/colors), TimingConfiguration (intervals),
               WiFiConfiguration (SSID/password), HardwareConfiguration (display specs).
*/
#include <vector>
#include <time.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ESP.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#ifndef APP_CPU_NUM
#define APP_CPU_NUM 1
#endif
#include "config/UserConfiguration.h"
#include "config/RuntimeSettings.h"
#include "config/WiFiConfiguration.h"
#include "config/TimingConfiguration.h"
#include "adapters/OpenSkyFetcher.h"
#include "adapters/AeroAPIFetcher.h"
#include "core/FlightDataFetcher.h"
#include "adapters/NeoMatrixDisplay.h"
#include "utils/NetLock.h"

RTC_DATA_ATTR static uint32_t g_resetCounter = 0;
#ifndef FW_BUILD_ID
#define FW_BUILD_ID __DATE__ " " __TIME__
#endif
static const char *const BUILD_ID = FW_BUILD_ID;
static WebServer g_server(80);
static bool g_restartAfterConfig = false;
static bool g_serverActive = false;
static bool g_serverVisited = false;
static unsigned long g_serverStartMs = 0;

static OpenSkyFetcher g_openSky;
static AeroAPIFetcher g_aeroApi;
static FlightDataFetcher *g_fetcher = nullptr;
static NeoMatrixDisplay g_display;
static std::vector<FlightInfo> g_lastFlights;
static SemaphoreHandle_t g_flightsMutex = nullptr;
static TaskHandle_t g_fetchTaskHandle = nullptr;

static unsigned long g_lastFetchMs = 0;
static unsigned long g_lastWifiCheckMs = 0;
static bool g_doubleResetWindowArmed = false;
static unsigned long g_doubleResetWindowStartMs = 0;

static void netDiag()
{
    Serial.println("--- net diag ---");
    Serial.printf("WiFi.status: %d (WL_CONNECTED=3)\n", WiFi.status());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("GW: %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("DNS: %s / %s\n",
                  WiFi.dnsIP(0).toString().c_str(),
                  WiFi.dnsIP(1).toString().c_str());

    IPAddress resolved;
    bool dnsOk = WiFi.hostByName("google.com", resolved) == 1;
    Serial.printf("hostByName(google.com): %s (%s)\n",
                  dnsOk ? "OK" : "FAIL",
                  dnsOk ? resolved.toString().c_str() : "-");

    WiFiClient client;
    bool tcpOk = client.connect("1.1.1.1", 80);
    Serial.printf("TCP to 1.1.1.1:80: %s\n", tcpOk ? "OK" : "FAIL");
    if (tcpOk)
    {
        client.stop();
    }
    Serial.println("--- end net diag ---");
}

static void maybeLogNetDiag(size_t stateCount, size_t flightCount)
{
    static int consecutiveEmpty = 0;
    static unsigned long lastDiagMs = 0;
    const unsigned long now = millis();

    if (stateCount == 0 && flightCount == 0)
    {
        consecutiveEmpty++;
    }
    else
    {
        consecutiveEmpty = 0;
    }

    const bool wifiDown = WiFi.status() != WL_CONNECTED;
    const bool dataStuck = consecutiveEmpty >= 2;
    const unsigned long DIAG_COOLDOWN_MS = 60000UL;

    if ((wifiDown || dataStuck) && (now - lastDiagMs >= DIAG_COOLDOWN_MS))
    {
        Serial.println(wifiDown
                           ? "NetDiag: WiFi disconnected; dumping network status"
                           : "NetDiag: No flights/weather twice in a row; dumping network status");
        netDiag();
        lastDiagMs = now;
    }
}

static void ensureWifiConnected()
{
    const unsigned long now = millis();
    const unsigned long CHECK_EVERY_MS = 10000;
    if (now - g_lastWifiCheckMs < CHECK_EVERY_MS)
        return;
    g_lastWifiCheckMs = now;

    bool badStatus = (WiFi.status() != WL_CONNECTED);
    bool missingIp = (WiFi.localIP().toString() == "0.0.0.0");
    if (badStatus || missingIp)
    {
        Serial.println("WiFi watchdog: connection lost; attempting reconnect");
        Serial.printf("Current status=%d, ip=%s\n", (int)WiFi.status(), WiFi.localIP().toString().c_str());
        WiFi.disconnect(true);
        delay(200);
        WiFi.begin(); // reconnect using stored credentials
    }
}

static void fetchTask(void *param)
{
    const TickType_t loopDelay = pdMS_TO_TICKS(50); // keep responsive while waiting for interval
    while (true)
    {
        const unsigned long intervalMs = TimingConfiguration::FETCH_INTERVAL_SECONDS * 1000UL;
        const unsigned long now = millis();
        ensureWifiConnected();
        if (g_fetcher != nullptr && now - g_lastFetchMs >= intervalMs)
        {
            g_lastFetchMs = now;

            std::vector<StateVector> states;
            std::vector<FlightInfo> flights;
            size_t enriched = g_fetcher->fetchFlights(states, flights);

            Serial.print("OpenSky state vectors: ");
            Serial.println((int)states.size());
            Serial.print("AeroAPI enriched flights: ");
            Serial.println((int)enriched);
            maybeLogNetDiag(states.size(), flights.size());

            if (g_flightsMutex && xSemaphoreTake(g_flightsMutex, pdMS_TO_TICKS(200)))
            {
                g_lastFlights = flights;
                xSemaphoreGive(g_flightsMutex);
            }
        }
        vTaskDelay(loopDelay);
    }
}

static String htmlEscape(const String &in)
{
    String out;
    for (size_t i = 0; i < in.length(); ++i)
    {
        char c = in[i];
        switch (c)
        {
        case '&': out += F("&amp;"); break;
        case '<': out += F("&lt;"); break;
        case '>': out += F("&gt;"); break;
        case '\"': out += F("&quot;"); break;
        case '\'': out += F("&#39;"); break;
        default: out += c; break;
        }
    }
    return out;
}

static String settingsPageHtml()
{
    const auto &cfg = RuntimeSettings::current();
    String html = F(
        "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:sans-serif;padding:12px;}label{display:block;margin:8px 0 4px;}input{width:100%;padding:8px;font-size:14px;}button{padding:10px 14px;margin-top:12px;}form{max-width:420px;}small{color:#555;}</style>"
        "<title>FlightWatch Settings</title></head><body><h2>FlightWatch Settings</h2><form method='POST' action='/save'>");
    auto addField = [&](const String &name, const String &label, const String &value, const String &hint) {
        html += "<label for='" + name + "'>" + label + "</label>";
        html += "<input id='" + name + "' name='" + name + "' value='" + htmlEscape(value) + "'>";
        if (hint.length())
        {
            html += "<small>" + hint + "</small>";
        }
    };

    addField("centerLat", "Center Latitude", String(cfg.centerLat, 6), "Example: 48.115452");
    addField("centerLon", "Center Longitude", String(cfg.centerLon, 6), "Example: 11.735858");
    addField("radiusKm", "Radius (km)", String(cfg.radiusKm, 2), "Example: 18.0");
    addField("weatherLat", "Weather Latitude", String(cfg.weatherLat, 6), "Leave same as center or override");
    addField("weatherLon", "Weather Longitude", String(cfg.weatherLon, 6), "");
    addField("tzIana", "Timezone (IANA)", cfg.timezoneIana, "Example: Europe/Berlin");
    addField("brightness", "Display Brightness (0-255)", String(cfg.displayBrightness), "");
    addField("aeroKey", "AeroAPI Key", cfg.aeroApiKey, "");
    addField("osId", "OpenSky Client ID", cfg.openSkyClientId, "");
    addField("osSecret", "OpenSky Client Secret", cfg.openSkyClientSecret, "");

    html += "<label for='altUnits'>Altitude Units</label>";
    html += "<select id='altUnits' name='altUnits'>";
    html += String("<option value='m'") + (cfg.altitudeFeet ? "" : " selected") + ">Meters</option>";
    html += String("<option value='ft'") + (cfg.altitudeFeet ? " selected" : "") + ">Feet</option>";
    html += "</select>";

    html += "<label for='speedUnits'>Speed Units</label>";
    html += "<select id='speedUnits' name='speedUnits'>";
    html += String("<option value='kmh'") + (cfg.speedKts ? "" : " selected") + ">km/h</option>";
    html += String("<option value='kts'") + (cfg.speedKts ? " selected" : "") + ">Knots</option>";
    html += "</select>";

    html += "<button type='submit'>Save</button></form>";
    html += "<form method='POST' action='/reset' onsubmit='return confirm(\"Reset to defaults?\");'><button type='submit'>Reset to defaults</button></form>";
    html += "</body></html>";
    return html;
}

static double parseDouble(const String &val, double fallback)
{
    if (val.length() == 0)
        return fallback;
    char *endptr = nullptr;
    double v = strtod(val.c_str(), &endptr);
    if (endptr == val.c_str())
        return fallback;
    return v;
}

static void handleSettingsSave()
{
    FlightWatchSettings updated = RuntimeSettings::current();
    updated.centerLat = parseDouble(g_server.arg("centerLat"), updated.centerLat);
    updated.centerLon = parseDouble(g_server.arg("centerLon"), updated.centerLon);
    updated.radiusKm = parseDouble(g_server.arg("radiusKm"), updated.radiusKm);
    updated.weatherLat = parseDouble(g_server.arg("weatherLat"), updated.centerLat);
    updated.weatherLon = parseDouble(g_server.arg("weatherLon"), updated.centerLon);
    updated.altitudeFeet = g_server.arg("altUnits") == "ft";
    updated.speedKts = g_server.arg("speedUnits") == "kts";

    long b = g_server.arg("brightness").toInt();
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    updated.displayBrightness = (uint8_t)b;

    updated.timezoneIana = g_server.arg("tzIana");
    updated.aeroApiKey = g_server.arg("aeroKey");
    updated.openSkyClientId = g_server.arg("osId");
    updated.openSkyClientSecret = g_server.arg("osSecret");

    if (!RuntimeSettings::save(updated))
    {
        g_server.send(500, "text/plain", "Failed to save settings");
        return;
    }

    g_server.send(200, "text/html", "<html><body><h3>Saved. Restarting...</h3></body></html>");
    delay(500);
    ESP.restart();
}

static void handleSettingsReset()
{
    Preferences prefs;
    prefs.begin("fwsettings", false);
    prefs.clear();
    prefs.end();
    RuntimeSettings::load(); // reload defaults
    g_server.send(200, "text/html", "<html><body><h3>Settings reset to defaults. Restarting...</h3></body></html>");
    delay(500);
    ESP.restart();
}

static void startSettingsServer()
{
    if (MDNS.begin("flightwatch"))
    {
        MDNS.addService("http", "tcp", 80);
    }
    g_server.on("/", HTTP_GET, []() {
        g_serverVisited = true;
        g_server.send(200, "text/html", settingsPageHtml());
    });
    g_server.on("/save", HTTP_POST, []() {
        g_serverVisited = true;
        handleSettingsSave();
    });
    g_server.on("/reset", HTTP_POST, []() {
        g_serverVisited = true;
        handleSettingsReset();
    });
    g_server.begin();
    Serial.println("Settings portal started at http://flightwatch.local/");
    g_serverActive = true;
    g_serverVisited = false;
    g_serverStartMs = millis();
}
static bool doubleResetDetected()
{
    g_resetCounter++;
    if (g_resetCounter > 1)
    {
        g_resetCounter = 0;
        return true;
    }
    g_doubleResetWindowArmed = true;
    g_doubleResetWindowStartMs = millis();
    return false;
}

static void serviceDoubleResetWindow()
{
    const unsigned long windowMs = WiFiConfiguration::DOUBLE_RESET_WINDOW_SECONDS * 1000UL;
    if (g_doubleResetWindowArmed && millis() - g_doubleResetWindowStartMs >= windowMs)
    {
        g_resetCounter = 0;
        g_doubleResetWindowArmed = false;
    }
}

void setup()
{
    Serial.begin(115200);
    delay(200);

    RuntimeSettings::load();
    g_flightsMutex = xSemaphoreCreateMutex();
    NetLock::init();

    g_display.initialize();
    g_display.displayStartup();
    delay(5000); // hold startup logo/text before entering WiFi setup

    // Ensure clean STA mode before WiFiManager (mirrors Clockwise setup)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);

    // Force fresh portal after new firmware flash by comparing build id persisted in NVS
    Preferences prefs;
    prefs.begin("fwcfg", false);
    String storedBuild = prefs.getString("build", "");
    bool isNewBuild = storedBuild != BUILD_ID;

    WiFiManager wifiManager;
    wifiManager.setDebugOutput(false);
    wifiManager.setConnectTimeout(WiFiConfiguration::CONNECT_TIMEOUT_SECONDS);
    wifiManager.setTimeout(WiFiConfiguration::PORTAL_TIMEOUT_SECONDS);
    wifiManager.setAPCallback([](WiFiManager *)
    {
        g_display.displayMessage(String("Setup: ") + WiFiConfiguration::PORTAL_SSID);
    });
    wifiManager.setSaveConfigCallback([]()
    {
        Serial.println("WiFiManager: credentials received, attempting connection");
        g_restartAfterConfig = true;
    });

    if (isNewBuild)
    {
        Serial.println("New firmware detected; clearing saved WiFi credentials");
        wifiManager.resetSettings();
        prefs.putString("build", BUILD_ID);
    }

    bool doubleReset = doubleResetDetected();
    bool wifiConnected = false;

    if (doubleReset)
    {
        Serial.println("Double reset detected; clearing WiFi credentials");
        g_display.displayMessage("WiFi reset...");
        wifiManager.resetSettings();
        wifiConnected = wifiManager.startConfigPortal(WiFiConfiguration::PORTAL_SSID, WiFiConfiguration::PORTAL_PASSWORD);
    }
    else
    {
        g_display.displayMessage("WiFi connect");
        wifiConnected = wifiManager.autoConnect(WiFiConfiguration::PORTAL_SSID, WiFiConfiguration::PORTAL_PASSWORD);

        if (!wifiConnected)
        {
            Serial.print("Stored WiFi failed; status=");
            Serial.println((int)WiFi.status());
            Serial.println("Opening portal...");
            g_display.displayMessage("Portal ready");
            wifiConnected = wifiManager.startConfigPortal(WiFiConfiguration::PORTAL_SSID, WiFiConfiguration::PORTAL_PASSWORD);
        }
    }

    if (g_restartAfterConfig && wifiConnected)
    {
        Serial.println("Restarting to apply new WiFi credentials...");
        ESP.restart();
    }

    if (wifiConnected)
    {
        Serial.print("WiFi connected: ");
        Serial.println(WiFi.localIP());
        g_display.displayMessage(String("WiFi OK ") + WiFi.localIP().toString());

        // Set timezone from runtime settings (POSIX string) and start NTP sync
        configTzTime(RuntimeSettings::current().timezonePosix.c_str(), "pool.ntp.org", "time.nist.gov");

        // Show logo/text once WiFi is up
        g_display.displayStartup();
        delay(5000);

        delay(1000);
        g_display.showLoading();

        // Start settings portal (MDNS + HTTP)
        startSettingsServer();
    }
    else
    {
        Serial.print("WiFi not connected; status=");
        Serial.println((int)WiFi.status());
        Serial.println("Proceeding without network");
        g_display.displayMessage(String("WiFi FAIL"));
    }

    g_fetcher = new FlightDataFetcher(&g_openSky, &g_aeroApi);
    if (g_fetchTaskHandle == nullptr)
    {
        xTaskCreatePinnedToCore(
            fetchTask,
            "fetchTask",
            8192,
            nullptr,
            1,
            &g_fetchTaskHandle,
            APP_CPU_NUM);
    }
}

void loop()
{
    serviceDoubleResetWindow();

    const unsigned long now = millis();

    // Copy latest flights under mutex to avoid blocking display during fetch
    std::vector<FlightInfo> flightsCopy;
    if (g_flightsMutex && xSemaphoreTake(g_flightsMutex, pdMS_TO_TICKS(5)))
    {
        flightsCopy = g_lastFlights;
        xSemaphoreGive(g_flightsMutex);
    }
    else
    {
        flightsCopy = g_lastFlights; // fallback if mutex unavailable
    }

    // Refresh display frequently so scrolling/cycling can progress independently of fetch cadence
    static unsigned long lastDisplayTickMs = 0;
    const unsigned long DISPLAY_TICK_MS = 25; // ~40 FPS
    if (now - lastDisplayTickMs >= DISPLAY_TICK_MS)
    {
        lastDisplayTickMs = now;
        g_display.displayFlights(flightsCopy);
    }
    if (g_serverActive)
    {
        g_server.handleClient();
        if (!g_serverVisited && millis() - g_serverStartMs > 10000UL)
        {
            Serial.println("Settings portal timeout; stopping server/MDNS");
            g_server.stop();
            MDNS.end();
            g_serverActive = false;
        }
    }
    delay(10);
}
