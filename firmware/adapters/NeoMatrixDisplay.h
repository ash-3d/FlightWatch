#pragma once

#include <stdint.h>
#include <vector>
#include "interfaces/BaseDisplay.h"

class MatrixPanel_I2S_DMA;

class NeoMatrixDisplay : public BaseDisplay
{
public:
    NeoMatrixDisplay();
    ~NeoMatrixDisplay() override;

    bool initialize() override;
    void clear() override;
    void displayFlights(const std::vector<FlightInfo> &flights) override;
    void displayMessage(const String &message);
    void displayStartup();
    void showLoading();

private:
    struct FlightCardLayout
    {
        String airline;
        int16_t airlineWidth = 0;
        int16_t airlineY = 0;

        String route;
        int16_t routeX = 0;
        int16_t routeY = 0;
        int16_t arrowX = 0;
        int16_t arrowY = 0;

        String modelLine1;
        String modelLine2;
        int16_t model1X = 0;
        int16_t model1Y = 0;
        int16_t model2X = 0;
        int16_t model2Y = 0;
        bool hasModel2 = false;

        String originName;
        String destName;
        int16_t originCityChars = 0;
        int16_t destCityChars = 0;
        int16_t originWidth = 0;
        int16_t destWidth = 0;
        int16_t cityArrowOffset = 0;
        bool originScrollActive = false;
        bool destScrollActive = false;
        int16_t originY = 0;
        int16_t destY = 0;
        bool showDest = false;

        String counter;
        int16_t counterX = 0;
        int16_t counterY = 0;
        bool showCounter = false;
    };

    MatrixPanel_I2S_DMA *_matrix = nullptr;

    uint16_t _matrixWidth = 0;
    uint16_t _matrixHeight = 0;

    size_t _currentFlightIndex = 0;
    unsigned long _lastCycleMs = 0;
    int _lastDisplayedFlightIndex = -1;

    // Cached layout to avoid recomputing strings every frame
    FlightCardLayout _layout;
    String _layoutKey;
    bool _layoutValid = false;
    String _lastIdent;
    String _lastIdentIata;
    String _lastIdentIcao;
    String _lastOriginCode;
    String _lastDestCode;
    String _lastAirlineFull;
    String _lastOperatorIata;
    String _lastOperatorIcao;
    String _lastAircraftDisplay;
    String _lastAircraftCode;
    size_t _lastLayoutOrdinal = 0;
    size_t _lastLayoutTotal = 0;
    bool _airlineScrollActive = false;
    int16_t _airlineScrollX = 0;
    unsigned long _lastAirlineScrollMs = 0;
    int16_t _originScrollX = 0;
    int16_t _destScrollX = 0;
    unsigned long _lastCityScrollMs = 0;

    float _lastTempC = NAN;
    unsigned long _lastTempFetchMs = 0;
    int _lastWeatherCode = -1;
    String _lastWeatherSymbol;
    uint16_t _lastWeatherColor = 0;

    void drawTextLine(int16_t x, int16_t y, const String &text, uint16_t color);
    String makeFlightLine(const FlightInfo &f);
    String truncateToColumns(const String &text, int maxColumns);
    String firstWord(const String &text) const;
    void displaySingleFlightCard(const FlightInfo &f);
    void runWipeTransition();
    void drawWeatherIcon(int16_t originX, int16_t originY, int weatherCode, uint16_t color);
    void displayLoadingScreen();
    String chooseAirlineName(const FlightInfo &f) const;
    String airportCodePreferred(const AirportInfo &a) const;
    String airportNamePreferred(const AirportInfo &a) const;
    String airportCity(const AirportInfo &a) const;
    bool fetchWeatherIfNeeded(float &outC, String &outSymbol, uint16_t &outColor);
    void present();
    void runBootTest();

    String flightCacheKey(const FlightInfo &f, size_t ordinal, size_t total) const;
    void prepareFlightLayout(const FlightInfo &f, size_t ordinal, size_t total);
    void updateAirlineScroll(unsigned long now);
    void updateCityScrolls(unsigned long now);

    void displaySingleFlightCard(const FlightInfo &f, size_t ordinal, size_t total);
};
