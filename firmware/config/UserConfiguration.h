#pragma once

#include <Arduino.h>

namespace UserConfiguration
{
    // Location configuration
    static const double CENTER_LAT = 48.1154525; // San Francisco (example)
    static const double CENTER_LON = 11.7358584;
    static const double RADIUS_KM = 18.0; // Search radius in km

    // Display customization
    // Brightness controls overall display brightness (0-255)
    static const uint8_t DISPLAY_BRIGHTNESS = 210;

    // RGB color for all text rendering on the LED matrix
    static const uint8_t TEXT_COLOR_R = 255;
    static const uint8_t TEXT_COLOR_G = 255;
    static const uint8_t TEXT_COLOR_B = 255;

    // Units (defaults)
    static const bool ALTITUDE_FEET = false; // false = meters, true = feet
    static const bool SPEED_KTS = false;     // false = km/h, true = knots

    // Timezone defaults
    static constexpr const char *TIMEZONE_IANA = "Europe/Berlin";
    // POSIX/TZ format. Example: Berlin CET/CEST
    static constexpr const char *TIMEZONE_TZ = "CET-1CEST,M3.5.0/2,M10.5.0/3";
}
