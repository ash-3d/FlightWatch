#pragma once

#include <Arduino.h>

namespace APIConfiguration
{
    // OpenSky API credentials (Basic Auth)
        static const char *OPENSKY_CLIENT_ID = "ashiktd@gmail.com-api-client";
    static const char *OPENSKY_CLIENT_SECRET = "y9oMTKxTNNsK3YOho3VhaFonOuvvoucB";
    static constexpr const char *OPENSKY_TOKEN_URL =
        "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token";

    static constexpr const char *OPENSKY_BASE_URL = "https://opensky-network.org";

    // FlightAware AeroAPI credentials
    static const char *AEROAPI_KEY = "bHeT8rDlRqBTHR4y5XxlBZuy2k0QYKM3";
    static constexpr const char *AEROAPI_BASE_URL =
        "https://aeroapi.flightaware.com/aeroapi";

    // FlightWall CDN lookup
    static constexpr const char *FLIGHTWALL_CDN_BASE_URL =
        "https://cdn.theflightwall.com";

    // TLS behavior for external services
    static const bool AEROAPI_INSECURE_TLS = true;
    static const bool FLIGHTWALL_INSECURE_TLS = true;
}
