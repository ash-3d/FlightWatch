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
#include "config/UserConfiguration.h"
#include "config/RuntimeSettings.h"
#include "config/WiFiConfiguration.h"
#include "config/TimingConfiguration.h"
#include "adapters/OpenSkyFetcher.h"
#include "adapters/AeroAPIFetcher.h"
#include "core/FlightDataFetcher.h"
#include "adapters/NeoMatrixDisplay.h"

RTC_DATA_ATTR static uint32_t g_resetCounter = 0;
#ifndef FW_BUILD_ID
#define FW_BUILD_ID __DATE__ " " __TIME__
#endif
static const char *const BUILD_ID = FW_BUILD_ID;
static WebServer g_server(80);
static bool g_restartAfterConfig = false;

static OpenSkyFetcher g_openSky;
static AeroAPIFetcher g_aeroApi;
static FlightDataFetcher *g_fetcher = nullptr;
static NeoMatrixDisplay g_display;
static std::vector<FlightInfo> g_lastFlights;

static unsigned long g_lastFetchMs = 0;
static bool g_doubleResetWindowArmed = false;
static unsigned long g_doubleResetWindowStartMs = 0;

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
        g_server.send(200, "text/html", settingsPageHtml());
    });
    g_server.on("/save", HTTP_POST, handleSettingsSave);
    g_server.on("/reset", HTTP_POST, handleSettingsReset);
    g_server.begin();
    Serial.println("Settings portal started at http://flightwatch.local/");
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
}

void loop()
{
    serviceDoubleResetWindow();

    const unsigned long intervalMs = TimingConfiguration::FETCH_INTERVAL_SECONDS * 1000UL;
    const unsigned long now = millis();
    if (now - g_lastFetchMs >= intervalMs)
    {
        g_lastFetchMs = now;

        std::vector<StateVector> states;
        std::vector<FlightInfo> flights;
        size_t enriched = g_fetcher->fetchFlights(states, flights);

        Serial.print("OpenSky state vectors: ");
        Serial.println((int)states.size());
        Serial.print("AeroAPI enriched flights: ");
        Serial.println((int)enriched);

        for (const auto &s : states)
        {
            Serial.print(" ");
            Serial.print(s.callsign);
            Serial.print(" @ ");
            Serial.print(s.distance_km, 1);
            Serial.print("km bearing ");
            Serial.println(s.bearing_deg, 1);
        }

        for (const auto &f : flights)
        {
            Serial.println("=== FLIGHT INFO ===");
            Serial.print("Ident: ");
            Serial.println(f.ident);
            Serial.print("Ident ICAO: ");
            Serial.println(f.ident_icao);
            Serial.print("Ident IATA: ");
            Serial.println(f.ident_iata);
            Serial.print("Airline: ");
            Serial.println(f.airline_display_name_full);
            Serial.print("Aircraft: ");
            Serial.println(f.aircraft_display_name_short.length() ? f.aircraft_display_name_short : f.aircraft_code);
            Serial.print("Operator Code: ");
            Serial.println(f.operator_code);
            Serial.print("Operator ICAO: ");
            Serial.println(f.operator_icao);
            Serial.print("Operator IATA: ");
            Serial.println(f.operator_iata);

            Serial.println("--- Origin ---");
            Serial.print("Code ICAO: ");
            Serial.println(f.origin.code_icao);

            Serial.println("--- Destination ---");
            Serial.print("Code ICAO: ");
            Serial.println(f.destination.code_icao);
            Serial.println("===================");
        }

        g_lastFlights = flights;
    }

    // Refresh display frequently so scrolling/cycling can progress independently of fetch cadence
    static unsigned long lastDisplayTickMs = 0;
    const unsigned long DISPLAY_TICK_MS = 25; // ~40 FPS
    if (now - lastDisplayTickMs >= DISPLAY_TICK_MS)
    {
        lastDisplayTickMs = now;
        g_display.displayFlights(g_lastFlights);
    }
    g_server.handleClient();
    delay(10);
}
