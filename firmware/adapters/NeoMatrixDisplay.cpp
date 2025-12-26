/*
Purpose: Render flight info on a HUB75 panel (ESP32 Trinity) via MatrixPanel_I2S_DMA.
Responsibilities:
- Initialize LED matrix based on HardwareConfiguration and user display settings.
- Render a simple two-line flight card (no border), using alternating frames:
    Frame A:
        Line 1: airline code from flight ident (e.g. "LH")
        Line 2: aircraft model (e.g. "A320", "747-8")
    Frame B:
        Line 1: origin (ICAO or "---")
        Line 2: destination (ICAO or "---")
- Show a minimal loading screen when no flights are available.
- Cycle through multiple flights at a configurable interval.
Inputs: FlightInfo list; UserConfiguration (colors/brightness), TimingConfiguration (cycle),
        HardwareConfiguration (dimensions/pin/tiling).
Outputs: Visual output to LED matrix using double-buffered DMA.
*/

#include "adapters/NeoMatrixDisplay.h"

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Adafruit_GFX.h>
#include <time.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config/UserConfiguration.h"
#include "config/RuntimeSettings.h"
#include "config/HardwareConfiguration.h"
#include "config/TimingConfiguration.h"
#include "images/flightwatch_logo.h"

namespace
{
    constexpr int CHAR_WIDTH = 6;
    constexpr int CHAR_HEIGHT = 8;
    constexpr int LINE_GAP = 2;
    constexpr int MARQUEE_GAP_PX = 10;
    constexpr unsigned long MARQUEE_FRAME_MS = 25; // 40 FPS target
    constexpr int MARQUEE_SPEED_PX = 1;
    constexpr int BORDER = 1;
    constexpr int PROGRESS_BAR_HEIGHT = 2;
}

NeoMatrixDisplay::NeoMatrixDisplay() {}

NeoMatrixDisplay::~NeoMatrixDisplay()
{
    if (_matrix)
    {
        delete _matrix;
        _matrix = nullptr;
    }
}

bool NeoMatrixDisplay::initialize()
{
    _matrixWidth  = HardwareConfiguration::DISPLAY_MATRIX_WIDTH;
    _matrixHeight = HardwareConfiguration::DISPLAY_MATRIX_HEIGHT;

    HUB75_I2S_CFG mxconfig(
        _matrixWidth,
        _matrixHeight,
        HardwareConfiguration::DISPLAY_CHAIN_LENGTH);

    mxconfig.gpio.e      = HardwareConfiguration::DISPLAY_GPIO_E;
    mxconfig.double_buff = true;

    _matrix = new MatrixPanel_I2S_DMA(mxconfig);
    if (_matrix == nullptr)
    {
        return false;
    }

    if (!_matrix->begin())
    {
        return false;
    }

    _matrix->setTextWrap(false);
    _matrix->setTextSize(1); // smallest built-in font
    _matrix->setBrightness8(RuntimeSettings::current().displayBrightness);

    runBootTest();
    clear();

    _currentFlightIndex = 0;
    _lastCycleMs = millis();
    return true;
}

void NeoMatrixDisplay::clear()
{
    if (_matrix)
    {
        _matrix->fillScreen(0);
        present();
    }
}

// Kept for compatibility with header (not used directly in UI)
String NeoMatrixDisplay::makeFlightLine(const FlightInfo &f)
{
    String airline = f.airline_display_name_full.length()
                         ? f.airline_display_name_full
                         : (f.operator_iata.length() ? f.operator_iata : f.operator_icao);
    if (airline.length() == 0)
    {
        airline = f.operator_code;
    }

    String origin = f.origin.code_icao;
    String dest   = f.destination.code_icao;
    String route  = origin + "-" + dest;
    String type   = f.aircraft_display_name_short.length()
                        ? f.aircraft_display_name_short
                        : f.aircraft_code;
    String ident  = f.ident.length() ? f.ident : f.ident_icao;

    String line = airline;
    if (ident.length())
    {
        line += " ";
        line += ident;
    }
    if (type.length())
    {
        line += " ";
        line += type;
    }
    if (route.length() > 1)
    {
        line += " ";
        line += route;
    }
    return line;
}

void NeoMatrixDisplay::drawTextLine(int16_t x, int16_t y,
                                    const String &text, uint16_t color)
{
    _matrix->setCursor(x, y);
    _matrix->setTextColor(color);
    for (size_t i = 0; i < (size_t)text.length(); ++i)
    {
        _matrix->write(text[i]);
    }
}

String NeoMatrixDisplay::truncateToColumns(const String &text, int maxColumns)
{
    if ((int)text.length() <= maxColumns)
        return text;
    if (maxColumns <= 3)
        return text.substring(0, maxColumns);
    return text.substring(0, maxColumns - 3) + String("...");
}

String NeoMatrixDisplay::firstWord(const String &text) const
{
    int space = text.indexOf(' ');
    if (space < 0)
        return text;
    return text.substring(0, space);
}

String NeoMatrixDisplay::chooseAirlineName(const FlightInfo &f) const
{
    if (f.airline_display_name_full.length())
        return f.airline_display_name_full;
    if (f.operator_iata.length())
        return f.operator_iata;
    if (f.operator_icao.length())
        return f.operator_icao;
    if (f.operator_code.length())
        return f.operator_code;
    if (f.ident_iata.length())
        return f.ident_iata;
    if (f.ident.length())
        return f.ident;
    return f.ident_icao;
}

String NeoMatrixDisplay::airportCodePreferred(const AirportInfo &a) const
{
    if (a.code_iata.length())
        return a.code_iata;
    if (a.code_icao.length())
        return a.code_icao;
    return String("---");
}

String NeoMatrixDisplay::airportNamePreferred(const AirportInfo &a) const
{
    if (a.name.length())
        return a.name;
    if (a.code_iata.length())
        return a.code_iata;
    if (a.code_icao.length())
        return a.code_icao;
    return String("Unknown");
}

String NeoMatrixDisplay::airportCity(const AirportInfo &a) const
{
    // Try to derive a city-like label from the airport name
    String name = a.name;
    int comma = name.indexOf(',');
    if (comma > 0)
    {
        name = name.substring(0, comma);
    }
    name.trim();

    // Strip common suffixes
    const char *suffixes[] = {" International Airport", " Intl Airport", " Intl", " Airport"};
    for (auto s : suffixes)
    {
        String suffix(s);
        if (name.endsWith(suffix))
        {
            name = name.substring(0, name.length() - suffix.length());
            name.trim();
            break;
        }
    }

    if (name.length())
        return name;
    if (a.code_iata.length())
        return a.code_iata;
    if (a.code_icao.length())
        return a.code_icao;
    return String("Unknown");
}

bool NeoMatrixDisplay::fetchWeatherIfNeeded(float &outC, String &outSymbol, uint16_t &outColor)
{
    const unsigned long CACHE_MS = 10UL * 60UL * 1000UL; // 10 minutes
    unsigned long now = millis();
    if (!isnan(_lastTempC) && _lastWeatherCode >= 0 && now - _lastTempFetchMs < CACHE_MS)
    {
        outC = _lastTempC;
        outSymbol = _lastWeatherSymbol;
        outColor = _lastWeatherColor;
        return true;
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    const auto &cfg = RuntimeSettings::current();
    String url = String("https://api.open-meteo.com/v1/forecast?latitude=") +
                 String(cfg.weatherLat, 6) +
                 "&longitude=" + String(cfg.weatherLon, 6) +
                 "&current=temperature_2m,weathercode";

    if (!http.begin(client, url))
        return false;

    int code = http.GET();
    if (code != 200)
    {
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err)
    {
        return false;
    }

    auto weatherAppearanceForCode = [&](int code, String &symbol, uint16_t &color) {
        // Map to human-readable labels and colors
        if (code == 0)
        {
            symbol = String("Sunny");
            color = _matrix->color565(255, 215, 0); // golden yellow
            return;
        }
        if (code == 1 || code == 2 || code == 3)
        {
            symbol = String("Cloudy");
            color = _matrix->color565(160, 160, 160); // gray
            return;
        }
        if (code == 45 || code == 48)
        {
            symbol = String("Fog");
            color = _matrix->color565(160, 160, 160); // gray
            return;
        }
        if (code >= 51 && code <= 55)
        {
            symbol = String("Drizzle");
            color = _matrix->color565(135, 206, 235); // sky blue
            return;
        }
        if (code >= 56 && code <= 57)
        {
            symbol = String("Freezing Drizzle");
            color = _matrix->color565(135, 206, 235);
            return;
        }
        if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82))
        {
            symbol = String("Rain");
            color = _matrix->color565(135, 206, 235);
            return;
        }
        if ((code >= 71 && code <= 77) || code == 85 || code == 86)
        {
            symbol = String("Snow");
            color = _matrix->color565(255, 255, 255); // white
            return;
        }
        symbol = String("Unknown");
        color = _matrix->color565(
            UserConfiguration::TEXT_COLOR_R,
            UserConfiguration::TEXT_COLOR_G,
            UserConfiguration::TEXT_COLOR_B);
    };

    bool ok = false;

    if (doc["current"]["temperature_2m"].is<float>())
    {
        _lastTempC = doc["current"]["temperature_2m"].as<float>();
        _lastTempFetchMs = now;
        outC = _lastTempC;
        ok = true;
    }

    if (doc["current"]["weathercode"].is<int>())
    {
        _lastWeatherCode = doc["current"]["weathercode"].as<int>();
        weatherAppearanceForCode(_lastWeatherCode, _lastWeatherSymbol, _lastWeatherColor);
        outSymbol = _lastWeatherSymbol;
        outColor = _lastWeatherColor;
        ok = true;
    }

    return ok;
}

void NeoMatrixDisplay::drawWeatherIcon(int16_t originX, int16_t originY, int weatherCode, uint16_t color)
{
    if (_matrix == nullptr)
        return;

    auto setPx = [&](int16_t dx, int16_t dy) {
        int16_t x = originX + dx;
        int16_t y = originY + dy;
        if (x >= 0 && x < _matrixWidth && y >= 0 && y < _matrixHeight)
        {
            _matrix->drawPixel(x, y, color);
        }
    };

    // Icon canvas ~8x8 pixels
    auto drawSun = [&]() {
        setPx(3, 3); // center
        setPx(2, 3); setPx(4, 3);
        setPx(3, 2); setPx(3, 4);
        setPx(2, 2); setPx(4, 2);
        setPx(2, 4); setPx(4, 4);
        setPx(3, 1); setPx(3, 5);
        setPx(1, 3); setPx(5, 3);
    };

    auto drawCloud = [&]() {
        // Upper puff
        setPx(2, 1); setPx(3, 0); setPx(4, 1);
        // Body
        setPx(1, 2); setPx(2, 2); setPx(3, 2); setPx(4, 2); setPx(5, 2);
        setPx(0, 3); setPx(1, 3); setPx(2, 3); setPx(3, 3); setPx(4, 3); setPx(5, 3); setPx(6, 3);
        setPx(1, 4); setPx(2, 4); setPx(3, 4); setPx(4, 4); setPx(5, 4);
    };

    auto drawRain = [&]() {
        drawCloud();
        setPx(2, 6);
        setPx(4, 6);
        setPx(6, 6);
    };

    auto drawSnow = [&]() {
        drawCloud();
        // simple snow dots under cloud
        setPx(2, 6);
        setPx(3, 7);
        setPx(4, 6);
    };

    auto drawFog = [&]() {
        setPx(0, 2); setPx(1, 2); setPx(2, 2); setPx(3, 2); setPx(4, 2); setPx(5, 2); setPx(6, 2);
        setPx(0, 4); setPx(1, 4); setPx(2, 4); setPx(3, 4); setPx(4, 4); setPx(5, 4); setPx(6, 4);
        setPx(0, 6); setPx(1, 6); setPx(2, 6); setPx(3, 6); setPx(4, 6); setPx(5, 6); setPx(6, 6);
    };

    auto drawUnknown = [&]() {
        setPx(3, 1); setPx(2, 2); setPx(4, 2); setPx(3, 3); setPx(3, 5); setPx(3, 7);
    };

    // Map weather code categories to icon
    if (weatherCode == 0)
    {
        drawSun();
        return;
    }
    if (weatherCode == 1 || weatherCode == 2 || weatherCode == 3 || weatherCode == 45 || weatherCode == 48)
    {
        drawCloud();
        return;
    }
    if ((weatherCode >= 51 && weatherCode <= 67) || (weatherCode >= 80 && weatherCode <= 82))
    {
        drawRain();
        return;
    }
    if ((weatherCode >= 71 && weatherCode <= 77) || weatherCode == 85 || weatherCode == 86)
    {
        drawSnow();
        return;
    }
    drawUnknown();
}


String NeoMatrixDisplay::flightCacheKey(const FlightInfo &f, size_t ordinal, size_t total) const
{
    String key;
    key.reserve(128);
    key += f.ident;
    key += '|';
    key += f.ident_icao;
    key += '|';
    key += f.ident_iata;
    key += '|';
    key += f.operator_code;
    key += '|';
    key += f.operator_iata;
    key += '|';
    key += f.operator_icao;
    key += '|';
    key += f.airline_display_name_full;
    key += '|';
    key += f.aircraft_display_name_short;
    key += '|';
    key += f.aircraft_code;
    key += '|';
    key += f.origin.code_iata;
    key += '|';
    key += f.origin.code_icao;
    key += '|';
    key += f.destination.code_iata;
    key += '|';
    key += f.destination.code_icao;
    key += '|';
    key += String(ordinal);
    key += '/';
    key += String(total);
    key += '|';
    key += String(f.baro_altitude_m, 1);
    key += '|';
    key += String(f.velocity_mps, 1);
    const auto &cfg = RuntimeSettings::current();
    key += '|';
    key += cfg.altitudeFeet ? String("ft") : String("m");
    key += '|';
    key += cfg.speedKts ? String("kts") : String("kmh");
    return key;
}

void NeoMatrixDisplay::prepareFlightLayout(const FlightInfo &f, size_t ordinal, size_t total)
{
    const int viewWidth = _matrixWidth - 2 * BORDER;
    const int viewHeight = _matrixHeight - 2 * BORDER;
    const int maxCols = viewWidth / CHAR_WIDTH;
    String originCode = airportCodePreferred(f.origin);
    String destCode   = airportCodePreferred(f.destination);

    bool sameFlight = _layoutValid &&
                      _lastLayoutOrdinal == ordinal &&
                      _lastLayoutTotal == total &&
                      _lastIdent == f.ident &&
                      _lastIdentIata == f.ident_iata &&
                      _lastIdentIcao == f.ident_icao &&
                      _lastOriginCode == originCode &&
                      _lastDestCode == destCode &&
                      _lastAirlineFull == f.airline_display_name_full &&
                      _lastOperatorIata == f.operator_iata &&
                      _lastOperatorIcao == f.operator_icao &&
                      _lastAircraftDisplay == f.aircraft_display_name_short &&
                      _lastAircraftCode == f.aircraft_code;
    if (sameFlight)
    {
        return;
    }

    _layoutKey = flightCacheKey(f, ordinal, total);
    _layoutValid = true;
    _lastLayoutOrdinal = ordinal;
    _lastLayoutTotal = total;
    _lastIdent = f.ident;
    _lastIdentIata = f.ident_iata;
    _lastIdentIcao = f.ident_icao;
    _lastOriginCode = originCode;
    _lastDestCode = destCode;
    _lastAirlineFull = f.airline_display_name_full;
    _lastOperatorIata = f.operator_iata;
    _lastOperatorIcao = f.operator_icao;
    _lastAircraftDisplay = f.aircraft_display_name_short;
    _lastAircraftCode = f.aircraft_code;

    _layout.airline = chooseAirlineName(f);
    if (_layout.airline.length() == 0)
    {
        _layout.airline = String("Unknown");
    }
    _layout.airlineWidth = _layout.airline.length() * CHAR_WIDTH;
    _layout.airlineY = BORDER + PROGRESS_BAR_HEIGHT + 1;
    _airlineScrollActive = _layout.airlineWidth > viewWidth;
    _airlineScrollX = BORDER;
    _lastAirlineScrollMs = millis();

    String routeGap = String("   "); // add extra spacing so arrow tip does not touch destination
    _layout.route = truncateToColumns(originCode + routeGap + destCode, maxCols);
    _layout.routeX = BORDER + (viewWidth - (int)_layout.route.length() * CHAR_WIDTH) / 2;
    if (_layout.routeX < BORDER) _layout.routeX = BORDER;
    _layout.routeY = _layout.airlineY + CHAR_HEIGHT + LINE_GAP + 2;
    int originChars = originCode.length();
    _layout.arrowX = _layout.routeX + originChars * CHAR_WIDTH + CHAR_WIDTH; // one character gap before arrow
    _layout.arrowY = _layout.routeY; // fits within the 8px text row

    auto detectMaker = [&](const String &code, const String &display) -> String {
        String first = firstWord(display);
        String lower = first;
        lower.toLowerCase();
        if (lower == "airbus") return String("Airbus");
        if (lower == "boeing") return String("Boeing");
        if (lower == "bombardier") return String("Bombardier");
        if (lower == "embraer") return String("Embraer");
        if (lower == "atr") return String("ATR");
        if (lower == "cessna") return String("Cessna");
        if (lower == "gulfstream") return String("Gulfstream");
        if (lower == "dassault") return String("Dassault");

        String upCode = code;
        upCode.toUpperCase();
        if (upCode.startsWith("A3") || upCode.startsWith("A2") || upCode.startsWith("A1"))
            return String("Airbus");
        if (upCode.startsWith("B7") || upCode.startsWith("B3") || upCode.startsWith("B2"))
            return String("Boeing");
        if (upCode.startsWith("CRJ") || upCode.startsWith("CL") || upCode.startsWith("DH"))
            return String("Bombardier");
        if (upCode.startsWith("E1") || upCode.startsWith("E2") || upCode.startsWith("ERJ"))
            return String("Embraer");
        if (upCode.startsWith("AT"))
            return String("ATR");
        return String("");
    };

    String displayModel = f.aircraft_display_name_short.length()
                              ? f.aircraft_display_name_short
                              : f.aircraft_code;
    if (displayModel.length() == 0)
        displayModel = "Unknown";
    displayModel.trim();

    String maker = detectMaker(f.aircraft_code, displayModel);
    String modelOnly = displayModel;
    if (maker.length() && modelOnly.startsWith(maker))
    {
        modelOnly = modelOnly.substring(maker.length());
        modelOnly.trim();
        if (modelOnly.length() == 0)
            modelOnly = displayModel;
    }

    String combined = maker.length() ? (maker + String(" ") + modelOnly) : modelOnly;
    bool combinedFits = ((int)combined.length() * CHAR_WIDTH) <= viewWidth;
    int16_t modelY = _layout.routeY + CHAR_HEIGHT + LINE_GAP;

    _layout.hasModel2 = false;
    if (combinedFits)
    {
        _layout.modelLine1 = truncateToColumns(combined, maxCols);
        _layout.model1X = BORDER + (viewWidth - (int)_layout.modelLine1.length() * CHAR_WIDTH) / 2;
        if (_layout.model1X < BORDER) _layout.model1X = BORDER;
        _layout.model1Y = modelY;
    }
    else
    {
        String makerLine = maker.length() ? maker : firstWord(modelOnly);
        makerLine = truncateToColumns(makerLine, maxCols);
        _layout.modelLine1 = makerLine;
        _layout.model1X = BORDER + (viewWidth - (int)makerLine.length() * CHAR_WIDTH) / 2;
        if (_layout.model1X < BORDER) _layout.model1X = BORDER;
        _layout.model1Y = modelY;

        String modelLine = truncateToColumns(modelOnly, maxCols);
        _layout.modelLine2 = modelLine;
        _layout.model2X = BORDER + (viewWidth - (int)modelLine.length() * CHAR_WIDTH) / 2;
        if (_layout.model2X < BORDER) _layout.model2X = BORDER;
        _layout.model2Y = modelY + CHAR_HEIGHT + 1;
        _layout.hasModel2 = (_layout.model2Y + CHAR_HEIGHT <= (_matrixHeight - BORDER));
    }

    String originFull = airportCity(f.origin);
    originFull.trim();
    if (!originFull.length())
        originFull = String("---");
    String destFull = airportCity(f.destination);
    destFull.trim();
    if (!destFull.length())
        destFull = String("---");

    String cityLine = originFull + String("   ") + destFull;
    const int originCityChars = originFull.length();
    const int destCityChars = destFull.length();
    const int cityGapChars = 3;

    auto chooseCallsign = [&]() -> String {
        if (f.ident_iata.length()) return f.ident_iata;
        if (f.ident.length()) return f.ident;
        if (f.ident_icao.length()) return f.ident_icao;
        return String("--");
    };

    String altStr("--");
    if (!isnan(f.baro_altitude_m))
    {
        const auto &cfg = RuntimeSettings::current();
        if (cfg.altitudeFeet)
        {
            long altFeet = lround(f.baro_altitude_m * 3.28084);
            altStr = String(altFeet) + String("ft");
        }
        else
        {
            long altMeters = lround(f.baro_altitude_m);
            altStr = String(altMeters) + String("m");
        }
    }

    String speedStr("--");
    if (!isnan(f.velocity_mps))
    {
        const auto &cfg = RuntimeSettings::current();
        if (cfg.speedKts)
        {
            long kts = lround(f.velocity_mps * 1.943844f);
            speedStr = String(kts) + String("kt");
        }
        else
        {
            long kmh = lround(f.velocity_mps * 3.6);
            speedStr = String(kmh) + String("km/h");
        }
    }

    String callsign = chooseCallsign();
    String metricsLine = callsign + String("  -  ") + altStr + String("  -  ") + speedStr;

    int16_t bottomY1 = BORDER + viewHeight - (2 * CHAR_HEIGHT) - LINE_GAP;
    if (bottomY1 < BORDER) bottomY1 = BORDER;
    _layout.originY = bottomY1;
    _layout.destY = bottomY1 + CHAR_HEIGHT + 1;
    _layout.showDest = (_layout.destY + CHAR_HEIGHT <= (_matrixHeight - BORDER));

    _layout.originName = cityLine;
    _layout.destName   = metricsLine;

    _layout.originWidth = _layout.originName.length() * CHAR_WIDTH;
    _layout.destWidth = _layout.destName.length() * CHAR_WIDTH;
    _layout.originCityChars = originCityChars;
    _layout.destCityChars = destCityChars;
    _layout.cityArrowOffset = originCityChars * CHAR_WIDTH + CHAR_WIDTH; // one character gap after origin
    _layout.originScrollActive = _layout.originWidth > viewWidth;
    _layout.destScrollActive = _layout.showDest && (_layout.destWidth > viewWidth);
    _originScrollX = BORDER;
    _destScrollX = BORDER;
    _lastCityScrollMs = millis();
}

void NeoMatrixDisplay::updateAirlineScroll(unsigned long now)
{
    if (!_airlineScrollActive || !_layoutValid)
        return;

    if (_lastAirlineScrollMs == 0)
    {
        _lastAirlineScrollMs = now;
    }

    if (now <= _lastAirlineScrollMs)
        return;

    unsigned long delta = now - _lastAirlineScrollMs;
    if (delta < MARQUEE_FRAME_MS)
        return;

    unsigned long steps = delta / MARQUEE_FRAME_MS;
    _lastAirlineScrollMs += steps * MARQUEE_FRAME_MS;

    const int viewWidth = _matrixWidth - 2 * BORDER;

    _airlineScrollX -= (int16_t)(steps * MARQUEE_SPEED_PX);
    int16_t reset = BORDER + viewWidth + MARQUEE_GAP_PX;
    int16_t minX = BORDER - (_layout.airlineWidth + MARQUEE_GAP_PX);
    if (_airlineScrollX < minX)
    {
        _airlineScrollX = reset;
    }
}

void NeoMatrixDisplay::updateCityScrolls(unsigned long now)
{
    bool anyScroll = (_layout.originScrollActive || _layout.destScrollActive);
    if (!anyScroll || !_layoutValid)
        return;

    const int viewWidth = _matrixWidth - 2 * BORDER;

    if (_lastCityScrollMs == 0)
    {
        _lastCityScrollMs = now;
    }

    if (now <= _lastCityScrollMs)
        return;

    unsigned long delta = now - _lastCityScrollMs;
    if (delta < MARQUEE_FRAME_MS)
        return;

    unsigned long steps = delta / MARQUEE_FRAME_MS;
    _lastCityScrollMs += steps * MARQUEE_FRAME_MS;

    auto advance = [&](int16_t &x, int16_t width) {
        x -= (int16_t)(steps * MARQUEE_SPEED_PX);
        int16_t reset = BORDER + viewWidth + MARQUEE_GAP_PX;
        int16_t minX = BORDER - (width + MARQUEE_GAP_PX);
        if (x < minX)
        {
            x = reset;
        }
    };

    if (_layout.originScrollActive)
    {
        advance(_originScrollX, _layout.originWidth);
    }
    if (_layout.destScrollActive)
    {
        advance(_destScrollX, _layout.destWidth);
    }
}

void NeoMatrixDisplay::displaySingleFlightCard(const FlightInfo &f, size_t ordinal, size_t total)
{
    const uint16_t textColor = _matrix->color565(
        UserConfiguration::TEXT_COLOR_R,
        UserConfiguration::TEXT_COLOR_G,
        UserConfiguration::TEXT_COLOR_B);
    const uint16_t originAccent = _matrix->color565(80, 200, 200); // soft teal
    const uint16_t destAccent = _matrix->color565(255, 200, 80);   // soft amber
    const uint16_t arrowColor = _matrix->color565(255, 255, 255);  // keep arrows white
    const uint16_t dimTextColor = _matrix->color565(
        UserConfiguration::TEXT_COLOR_R / 3,
        UserConfiguration::TEXT_COLOR_G / 3,
        UserConfiguration::TEXT_COLOR_B / 3);
    unsigned long now    = millis();

    prepareFlightLayout(f, ordinal, total);
    updateAirlineScroll(now);
    updateCityScrolls(now);

    _matrix->fillScreen(0);

    // Progress bar at top showing current flight when multiple flights
    const int viewWidth = _matrixWidth - 2 * BORDER;
    if (total > 1)
    {
        const int gap = 1;
        const int available = viewWidth - gap * (int)(total - 1);
        int baseWidth = available / (int)total;
        int remainder = available - baseWidth * (int)total;
        int16_t segmentX = BORDER;
        for (size_t i = 0; i < total; ++i)
        {
            int segWidth = baseWidth + (remainder > 0 ? 1 : 0);
            if (remainder > 0) remainder--;
            uint16_t color = (i == (ordinal - 1)) ? textColor : dimTextColor;
            _matrix->fillRect(segmentX, BORDER, segWidth, PROGRESS_BAR_HEIGHT, color);
            segmentX += segWidth + gap;
        }
    }

    int16_t airlineX = _airlineScrollActive ? _airlineScrollX : BORDER;
    drawTextLine(airlineX, _layout.airlineY, _layout.airline, textColor);
    if (_airlineScrollActive)
    {
        int16_t repeatX = airlineX + _layout.airlineWidth + MARQUEE_GAP_PX;
        if (repeatX < (_matrixWidth - BORDER))
        {
            drawTextLine(repeatX, _layout.airlineY, _layout.airline, textColor);
        }
    }

    // Draw route with per-segment colors
    int16_t routeDestX = _layout.routeX + (int16_t)(_lastOriginCode.length() * CHAR_WIDTH) + (int16_t)(3 * CHAR_WIDTH);
    drawTextLine(_layout.routeX, _layout.routeY, _lastOriginCode, originAccent);
    auto drawArrow = [&](int16_t x, int16_t y, uint16_t color) {
        // Solid right-pointing triangle, 6px wide, 7px tall
        _matrix->fillTriangle(
            x,     y,          // point A
            x,     y + 7,      // point B
            x + 6, y + 3,      // tip
            color);
    };
    drawArrow(_layout.arrowX, _layout.arrowY, arrowColor);
    drawTextLine(routeDestX, _layout.routeY, _lastDestCode, destAccent);
    drawTextLine(_layout.model1X, _layout.model1Y, _layout.modelLine1, textColor);
    if (_layout.hasModel2)
    {
        drawTextLine(_layout.model2X, _layout.model2Y, _layout.modelLine2, textColor);
    }

    int16_t originX = _layout.originScrollActive ? _originScrollX : BORDER;
    const int16_t cityDestOffset = (int16_t)(_layout.originCityChars * CHAR_WIDTH) + (int16_t)(3 * CHAR_WIDTH);

    drawTextLine(originX, _layout.originY, _layout.originName.substring(0, _layout.originCityChars), originAccent);
    drawArrow(originX + _layout.cityArrowOffset, _layout.originY, arrowColor);
    int16_t cityDestX = originX + cityDestOffset;
    drawTextLine(cityDestX, _layout.originY, _layout.originName.substring(_layout.originCityChars + 3), destAccent);
    if (_layout.originScrollActive)
    {
        int16_t repeatX = originX + _layout.originWidth + MARQUEE_GAP_PX;
        if (repeatX < (_matrixWidth - BORDER))
        {
            drawTextLine(repeatX, _layout.originY, _layout.originName.substring(0, _layout.originCityChars), originAccent);
            drawArrow(repeatX + _layout.cityArrowOffset, _layout.originY, arrowColor);
            int16_t cityDestX2 = repeatX + cityDestOffset;
            drawTextLine(cityDestX2, _layout.originY, _layout.originName.substring(_layout.originCityChars + 3), destAccent);
        }
    }

    if (_layout.showDest)
    {
        int16_t destX = _layout.destScrollActive ? _destScrollX : BORDER;
        drawTextLine(destX, _layout.destY, _layout.destName, textColor);
        if (_layout.destScrollActive)
        {
            int16_t repeatX = destX + _layout.destWidth + MARQUEE_GAP_PX;
            if (repeatX < (_matrixWidth - BORDER))
            {
                drawTextLine(repeatX, _layout.destY, _layout.destName, textColor);
            }
        }
    }

}

void NeoMatrixDisplay::displayFlights(const std::vector<FlightInfo> &flights)
{
    if (_matrix == nullptr)
        return;

    if (flights.empty())
    {
        _layoutValid = false;
        _layoutKey = String();
        displayLoadingScreen();
        return;
    }

    const unsigned long now        = millis();
    const unsigned long intervalMs = TimingConfiguration::DISPLAY_CYCLE_SECONDS * 1000UL;

    if (flights.size() > 1)
    {
        if (now - _lastCycleMs >= intervalMs)
        {
            _lastCycleMs = now;
            _currentFlightIndex = (_currentFlightIndex + 1) % flights.size();
        }
    }
    else
    {
        _currentFlightIndex = 0;
    }

    const size_t index = _currentFlightIndex % flights.size();
    if (_lastDisplayedFlightIndex >= 0 && _lastDisplayedFlightIndex != (int)index)
    {
        runWipeTransition();
    }
    displaySingleFlightCard(flights[index], index + 1, flights.size());
    _lastDisplayedFlightIndex = (int)index;
    present();
}

void NeoMatrixDisplay::displayLoadingScreen()
{
    if (_matrix == nullptr)
        return;

    _matrix->fillScreen(0);

    const int charWidth  = 6;
    const int charHeight = 8;
    const int lineGap    = 2;

    const uint16_t textColor = _matrix->color565(
        UserConfiguration::TEXT_COLOR_R,
        UserConfiguration::TEXT_COLOR_G,
        UserConfiguration::TEXT_COLOR_B);
    const uint16_t lightBlue = _matrix->color565(80, 140, 255);
    const uint16_t boisenberry = _matrix->color565(135, 50, 96);
    const uint16_t lavender = _matrix->color565(230, 230, 250);

    // Build time/date strings using RTC/NTP if available
    char timeBuf[16];
    char dateBuf[16];
    char dayBuf[16];
    struct tm t;
    bool timeValid = getLocalTime(&t, 0);
    if (timeValid)
    {
        strftime(timeBuf, sizeof(timeBuf), "%H:%M", &t);
        strftime(dateBuf, sizeof(dateBuf), "%d.%m.%Y", &t);
        strftime(dayBuf, sizeof(dayBuf), "%A", &t);
    }
    else
    {
        snprintf(timeBuf, sizeof(timeBuf), "--:--");
        snprintf(dateBuf, sizeof(dateBuf), "--.--.----");
        snprintf(dayBuf, sizeof(dayBuf), "------");
    }

    String timeStr(timeBuf);
    String dateStr(dateBuf);
    String dayStr(dayBuf);

    // Blink colon every second and render slightly bolder by double-drawing
    bool colonOn = ((millis() / 1000UL) % 2UL) == 0;
    String timeDisplay = timeStr;
    if (!colonOn)
    {
        timeDisplay.replace(":", " ");
    }
    const int glyphGap = 1; // 1px gap between digits
    int16_t timeX = 0;
    int16_t timeY = 1; // slight offset from the top edge
    int16_t drawX = timeX;
    for (size_t i = 0; i < (size_t)timeDisplay.length(); ++i)
    {
        String ch(timeDisplay[i]);
        drawTextLine(drawX, timeY, ch, lightBlue);
        drawTextLine(drawX + 1, timeY, ch, lightBlue); // pseudo-bold with 1px offset
        drawX += charWidth + glyphGap;
    }

    // Temperature in top-right, rounded to whole degrees, with text weather description under it
    float tempC = NAN;
    String weatherSymbol;
    uint16_t weatherColor = textColor;
    if (fetchWeatherIfNeeded(tempC, weatherSymbol, weatherColor))
    {
        if (!isnan(tempC))
        {
            long rounded = lroundf(tempC);
            char tempBuf[16];
            const char degChar = (char)248; // degree symbol in GLCD font
            snprintf(tempBuf, sizeof(tempBuf), "%ld%cC", rounded, degChar);
            String tempStr(tempBuf);
            int16_t tempX = _matrixWidth - (int)tempStr.length() * charWidth;
            if (tempX < 0) tempX = 0;
            drawTextLine(tempX, 0, tempStr, textColor);
        }

        // Weather text just below the temperature, right-aligned
        if (weatherSymbol.length())
        {
            int16_t symX = _matrixWidth - (int)weatherSymbol.length() * charWidth;
            if (symX < 0) symX = 0;
            int16_t symY = charHeight + lineGap;
            drawTextLine(symX, symY, weatherSymbol, weatherColor);
        }
    }

    // Day and date stacked in bottom-left
    int16_t dateY = _matrixHeight - charHeight;
    int16_t dayY = dateY - charHeight - lineGap;
    if (dayY < 0) dayY = 0;
    const int16_t dateX = 1; // nudge right to avoid enclosure lip
    drawTextLine(dateX, dayY, dayStr, boisenberry);
    drawTextLine(dateX, dateY, dateStr, lavender);

    present();
}

void NeoMatrixDisplay::displayMessage(const String &message)
{
    if (_matrix == nullptr)
        return;

    _matrix->fillScreen(0);

    const int charWidth  = 6;
    const int charHeight = 6;

    const uint16_t textColor = _matrix->color565(
        UserConfiguration::TEXT_COLOR_R,
        UserConfiguration::TEXT_COLOR_G,
        UserConfiguration::TEXT_COLOR_B);

    const int maxCols = _matrixWidth / charWidth;
    String line = truncateToColumns(message, maxCols);

    const int16_t x = 0;
    const int16_t y = (_matrixHeight - charHeight) / 2;
    drawTextLine(x, y, line, textColor);
    present();
}

void NeoMatrixDisplay::displayStartup()
{
    if (_matrix == nullptr)
        return;

    _matrix->fillScreen(0);

    const int charWidth  = 6;
    const int charHeight = 6;
    const uint16_t textColor = _matrix->color565(
        UserConfiguration::TEXT_COLOR_R,
        UserConfiguration::TEXT_COLOR_G,
        UserConfiguration::TEXT_COLOR_B);

    // Draw logo (64x64 mono) centered
    const int logoW = 64;
    const int logoH = 64;
    int16_t logoX = (_matrixWidth - logoW) / 2;
    if (logoX < 0) logoX = 0;
    int16_t logoY = (_matrixHeight - logoH) / 2;
    if (logoY < 0) logoY = 0;
    const uint8_t *logo = FLIGHTWATCH_LOGO_64x64;
    for (int y = 0; y < logoH && (logoY + y) < _matrixHeight; ++y)
    {
        for (int x = 0; x < logoW && (logoX + x) < _matrixWidth; ++x)
        {
            // Each byte holds 8 pixels, MSB first
            int byteIndex = (y * logoW + x) / 8;
            int bitIndex = 7 - (x % 8);
            bool white = (logo[byteIndex] >> bitIndex) & 0x01;
            if (!white)
            {
                _matrix->drawPixel(logoX + x, logoY + y, textColor);
            }
        }
    }

    present();
}

void NeoMatrixDisplay::showLoading()
{
    displayLoadingScreen();
}

void NeoMatrixDisplay::present()
{
    if (_matrix == nullptr)
        return;

    // With double buffering enabled, this pushes the back buffer to the panel.
    _matrix->flipDMABuffer();
}

void NeoMatrixDisplay::runWipeTransition()
{
    if (_matrix == nullptr)
        return;

    const int wipeWidth = 6; // pixel band; small to keep transition light
    for (int x = 0; x < _matrixWidth; x += wipeWidth)
    {
        _matrix->fillRect(x, 0, wipeWidth, _matrixHeight, 0);
        present();
        delay(8); // brief delay to show movement without stressing refresh
    }
    _matrix->fillScreen(0);
    present();
}

void NeoMatrixDisplay::runBootTest()
{
    if (_matrix == nullptr)
        return;

    const uint16_t red   = _matrix->color565(255, 0, 0);
    const uint16_t green = _matrix->color565(0, 255, 0);
    const uint16_t blue  = _matrix->color565(0, 0, 255);
    const uint16_t white = _matrix->color565(255, 255, 255);

    uint16_t colors[] = {red, green, blue, white};
    for (uint8_t i = 0; i < 4; ++i)
    {
        _matrix->fillScreen(colors[i]);
        present();
        delay(1000);
    }

    // Checkerboard using drawPixel
    _matrix->fillScreen(0);
    for (uint16_t y = 0; y < _matrixHeight; ++y)
    {
        for (uint16_t x = 0; x < _matrixWidth; ++x)
        {
            if (((x + y) & 1) == 0)
            {
                _matrix->drawPixel(x, y, white);
            }
        }
    }
    present();
    delay(1000);
}
