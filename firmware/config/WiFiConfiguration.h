#pragma once

#include <Arduino.h>

namespace WiFiConfiguration
{
    // WiFiManager captive portal defaults
    static const char *PORTAL_SSID = "FlightWatch-Setup";
    static const char *PORTAL_PASSWORD = "12345678";

    // Timeouts (seconds)
    static const uint16_t CONNECT_TIMEOUT_SECONDS = 20;  // Wait for stored creds before falling back
    static const uint16_t PORTAL_TIMEOUT_SECONDS = 180;  // Leave portal up before giving up
    static const uint16_t DOUBLE_RESET_WINDOW_SECONDS = 10; // Reset creds if two resets occur quickly
}
