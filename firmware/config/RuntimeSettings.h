#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "config/UserConfiguration.h"
#include "config/APIConfiguration.h"

struct FlightWatchSettings
{
    double centerLat;
    double centerLon;
    double radiusKm;

    double weatherLat;
    double weatherLon;

    uint8_t displayBrightness;
    uint8_t textColorR;
    uint8_t textColorG;
    uint8_t textColorB;
    bool altitudeFeet;
    bool speedKts;

    String timezoneIana;
    String timezonePosix;

    String aeroApiKey;
    String openSkyClientId;
    String openSkyClientSecret;
};

namespace RuntimeSettings
{
    void load();
    bool save(const FlightWatchSettings &newSettings);
    const FlightWatchSettings &current();
}
