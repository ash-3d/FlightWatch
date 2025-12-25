#pragma once

#include <Arduino.h>

namespace TimingConfiguration
{
    // Fetch cadence (seconds) limited by 4000 monthly requests to OpenSky
    static const uint32_t FETCH_INTERVAL_SECONDS = 30; // seconds

    // Display cycling configuration
    static const uint32_t DISPLAY_CYCLE_SECONDS = 3; // seconds per flight when multiple flights
}
