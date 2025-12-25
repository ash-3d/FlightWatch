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
    MatrixPanel_I2S_DMA *_matrix = nullptr;

    uint16_t _matrixWidth = 0;
    uint16_t _matrixHeight = 0;

    size_t _currentFlightIndex = 0;
    unsigned long _lastCycleMs = 0;
    int _lastDisplayedFlightIndex = -1;

    // NEW: scrolling state for small displays
    int16_t _scrollX = 0;
    unsigned long _lastScrollMs = 0;
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

    void displaySingleFlightCard(const FlightInfo &f, size_t ordinal, size_t total);
};
