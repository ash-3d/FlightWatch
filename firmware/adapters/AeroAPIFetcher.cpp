/*
Purpose: Retrieve detailed flight metadata from AeroAPI over HTTPS.
Responsibilities:
- Perform authenticated GET to /flights/{ident} using API key.
- Parse minimal fields into FlightInfo (ident/operator/aircraft and ICAO codes).
- Handle TLS (optionally insecure for dev) and JSON errors gracefully.
Input: flight ident (e.g., callsign).
Output: Populates FlightInfo on success and returns true.
*/
#include "adapters/AeroAPIFetcher.h"
#include "config/RuntimeSettings.h"

static String safeGetString(JsonVariantConst v, const char *key)
{
    JsonVariantConst val = v[key];
    if (val.isNull())
        return String("");
    return String(val.as<const char *>());
}

bool AeroAPIFetcher::fetchFlightInfo(const String &flightIdent, FlightInfo &outInfo)
{
    const auto &cfg = RuntimeSettings::current();
    if (cfg.aeroApiKey.length() == 0)
    {
        Serial.println("AeroAPIFetcher: No API key configured");
        return false;
    }

    WiFiClientSecure client;
    if (APIConfiguration::AEROAPI_INSECURE_TLS)
    {
        client.setInsecure();
    }

    HTTPClient http;
    String url = String(APIConfiguration::AEROAPI_BASE_URL) + "/flights/" + flightIdent;
    http.begin(client, url);
    http.addHeader("x-apikey", cfg.aeroApiKey);
    http.addHeader("Accept", "application/json");

    int code = http.GET();
    if (code != 200)
    {
        Serial.printf("AeroAPIFetcher: HTTP request failed with code %d for flight %s\n", code, flightIdent.c_str());
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    // Filter to only keep fields we actually use; reduces RAM needs on large responses.
    StaticJsonDocument<512> filter;
    filter["flights"][0]["ident"] = true;
    filter["flights"][0]["ident_icao"] = true;
    filter["flights"][0]["ident_iata"] = true;
    filter["flights"][0]["operator"] = true;
    filter["flights"][0]["operator_icao"] = true;
    filter["flights"][0]["operator_iata"] = true;
    filter["flights"][0]["aircraft_type"] = true;
    filter["flights"][0]["origin"]["code_icao"] = true;
    filter["flights"][0]["origin"]["code_iata"] = true;
    filter["flights"][0]["origin"]["name"] = true;
    filter["flights"][0]["destination"]["code_icao"] = true;
    filter["flights"][0]["destination"]["code_iata"] = true;
    filter["flights"][0]["destination"]["name"] = true;

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (err)
    {
        Serial.printf("AeroAPIFetcher: JSON parsing failed for flight %s: %s\n", flightIdent.c_str(), err.c_str());
        return false;
    }

    JsonArray flights = doc["flights"].as<JsonArray>();
    if (flights.isNull() || flights.size() == 0)
    {
        Serial.printf("AeroAPIFetcher: No flights found in response for %s\n", flightIdent.c_str());
        return false;
    }

    JsonObject f = flights[0].as<JsonObject>();
    outInfo.ident = safeGetString(f, "ident");
    outInfo.ident_icao = safeGetString(f, "ident_icao");
    outInfo.ident_iata = safeGetString(f, "ident_iata");
    outInfo.operator_code = safeGetString(f, "operator");
    outInfo.operator_icao = safeGetString(f, "operator_icao");
    outInfo.operator_iata = safeGetString(f, "operator_iata");
    outInfo.aircraft_code = safeGetString(f, "aircraft_type");

    if (f["origin"].is<JsonObject>())
    {
        JsonObject o = f["origin"].as<JsonObject>();
        outInfo.origin.code_icao = safeGetString(o, "code_icao");
        outInfo.origin.code_iata = safeGetString(o, "code_iata");
        outInfo.origin.name = safeGetString(o, "name");
    }

    if (f["destination"].is<JsonObject>())
    {
        JsonObject d = f["destination"].as<JsonObject>();
        outInfo.destination.code_icao = safeGetString(d, "code_icao");
        outInfo.destination.code_iata = safeGetString(d, "code_iata");
        outInfo.destination.name = safeGetString(d, "name");
    }

    // Debug: log operator fields a few times to verify presence/format.
    static int s_opLogCount = 0;
    if (s_opLogCount < 5)
    {
        Serial.printf("AeroAPI debug ident=%s operator_icao=%s operator=%s aircraft_type=%s\n",
                      flightIdent.c_str(),
                      outInfo.operator_icao.c_str(),
                      outInfo.operator_code.c_str(),
                      outInfo.aircraft_code.c_str());
        s_opLogCount++;
    }

    return true;
}
