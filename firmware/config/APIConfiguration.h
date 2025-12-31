#pragma once

#include <Arduino.h>

#ifdef __has_include
#if __has_include("APIConfiguration.local.h")
#define API_CONFIGURATION_HAS_LOCAL 1
#include "APIConfiguration.local.h"
#endif
#endif

#ifndef API_CONFIGURATION_HAS_LOCAL
namespace APIConfiguration
{
    // OpenSky API credentials (Basic Auth) – set real values in APIConfiguration.local.h
    static const char *OPENSKY_CLIENT_ID = "<opensky-client-id>";
    static const char *OPENSKY_CLIENT_SECRET = "<opensky-client-secret>";
    static constexpr const char *OPENSKY_TOKEN_URL =
        "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token";

    static constexpr const char *OPENSKY_BASE_URL = "https://opensky-network.org";

    // FlightAware AeroAPI credentials
    static const char *AEROAPI_KEY = "<aeroapi-key>";
    static constexpr const char *AEROAPI_BASE_URL =
        "https://aeroapi.flightaware.com/aeroapi";

    // TLS behavior for external services
    static const bool AEROAPI_INSECURE_TLS = true;
}
#endif
