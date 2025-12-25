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
#include "config/HardwareConfiguration.h"
#include "config/TimingConfiguration.h"

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
    _matrix->setBrightness8(UserConfiguration::DISPLAY_BRIGHTNESS);

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

    String url = String("https://api.open-meteo.com/v1/forecast?latitude=") +
                 String(UserConfiguration::CENTER_LAT, 6) +
                 "&longitude=" + String(UserConfiguration::CENTER_LON, 6) +
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

void NeoMatrixDisplay::displaySingleFlightCard(const FlightInfo &f, size_t ordinal, size_t total)
{
    // Text color
    const uint16_t textColor = _matrix->color565(
        UserConfiguration::TEXT_COLOR_R,
        UserConfiguration::TEXT_COLOR_G,
        UserConfiguration::TEXT_COLOR_B);

    // Default 5x7 font with 1px spacing => approx 6x8 per char
    const int charWidth  = 6;
    const int charHeight = 8;
    const int lineGap    = 2;
    unsigned long now    = millis();

    _matrix->fillScreen(0);

    // Max characters that fit in one line (e.g. 32 / 6 ~= 10 chars on 64px wide display)
    const int maxCols = _matrixWidth / charWidth;

    // Airline: first word only, clipped to fit, drawn top-left
    String airlineName = firstWord(chooseAirlineName(f));
    airlineName = truncateToColumns(airlineName, maxCols);
    int16_t airlineX = 0;
    int16_t airlineY = 0;
    drawTextLine(airlineX, airlineY, airlineName, textColor);

    // Route (IATA preferred) centered
    String originCode = airportCodePreferred(f.origin);
    String destCode   = airportCodePreferred(f.destination);
    String route      = originCode + String(" > ") + destCode;
    route = truncateToColumns(route, maxCols);
    int16_t routeX = (_matrixWidth - (int)route.length() * charWidth) / 2;
    if (routeX < 0) routeX = 0;
    int16_t routeY = charHeight + lineGap + 2;
    drawTextLine(routeX, routeY, route, textColor);

    // Aircraft maker + model
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
    bool combinedFits = ((int)combined.length() * charWidth) <= _matrixWidth;
    int16_t modelY = routeY + charHeight + lineGap;

    if (combinedFits)
    {
        String modelLine = truncateToColumns(combined, maxCols);
        int16_t modelX = (_matrixWidth - (int)modelLine.length() * charWidth) / 2;
        if (modelX < 0) modelX = 0;
        drawTextLine(modelX, modelY, modelLine, textColor);
    }
    else
    {
        String makerLine = maker.length() ? maker : firstWord(modelOnly);
        makerLine = truncateToColumns(makerLine, maxCols);
        int16_t makerX = (_matrixWidth - (int)makerLine.length() * charWidth) / 2;
        if (makerX < 0) makerX = 0;
        drawTextLine(makerX, modelY, makerLine, textColor);

        String modelLine = truncateToColumns(modelOnly, maxCols);
        int16_t modelX2 = (_matrixWidth - (int)modelLine.length() * charWidth) / 2;
        if (modelX2 < 0) modelX2 = 0;
        int16_t modelY2 = modelY + charHeight + 1;
        if (modelY2 + charHeight <= _matrixHeight)
        {
            drawTextLine(modelX2, modelY2, modelLine, textColor);
        }
    }

    // Airport names at bottom-left (city focused)
    String originName = firstWord(airportCity(f.origin));
    String destName   = firstWord(airportCity(f.destination));
    originName = truncateToColumns(originName, maxCols);
    destName   = truncateToColumns(destName, maxCols);

    int16_t bottomY1 = _matrixHeight - (2 * charHeight) - lineGap;
    if (bottomY1 < 0) bottomY1 = 0;
    int16_t bottomY2 = bottomY1 + charHeight + 1;
    drawTextLine(0, bottomY1, originName, textColor);
    if (bottomY2 + charHeight <= _matrixHeight)
    {
        drawTextLine(0, bottomY2, destName, textColor);
    }

    // Flight counter (e.g., "1/3") bottom-right
    if (total > 1)
    {
        String counter = String(ordinal) + String("/") + String(total);
        int counterWidth = counter.length() * charWidth;
        int16_t counterX = _matrixWidth - counterWidth;
        if (counterX < 0) counterX = 0;
        int16_t counterY = _matrixHeight - charHeight;
        drawTextLine(counterX, counterY, counter, textColor);
    }
}

void NeoMatrixDisplay::displayFlights(const std::vector<FlightInfo> &flights)
{
    if (_matrix == nullptr)
        return;

    if (flights.empty())
    {
        displayLoadingScreen();
        return;
    }

    _matrix->fillScreen(0);

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
    drawTextLine(0, dayY, dayStr, boisenberry);
    drawTextLine(0, dateY, dateStr, lavender);

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
