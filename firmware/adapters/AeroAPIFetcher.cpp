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
#include "utils/NetLock.h"

static unsigned long s_lastTlsFailMs = 0;
static const unsigned long kTlsBackoffMs = 20000UL; // back off 20s after TLS alloc failure

static String safeGetString(JsonVariantConst v, const char *key)
{
    JsonVariantConst val = v[key];
    if (val.isNull())
        return String("");
    return String(val.as<const char *>());
}

bool AeroAPIFetcher::fetchFlightInfo(const String &flightIdent, FlightInfo &outInfo)
{
    unsigned long nowMs = millis();
    if (s_lastTlsFailMs != 0 && nowMs - s_lastTlsFailMs < kTlsBackoffMs)
    {
        Serial.println("AeroAPIFetcher: backing off after TLS failure");
        return false;
    }

    NetLock::Guard guard(8000); // higher priority than weather; wait for lock
    if (!guard.locked())
    {
        Serial.println("AeroAPIFetcher: network busy, skipping fetch");
        return false;
    }

    const auto &cfg = RuntimeSettings::current();
    if (cfg.aeroApiKey.length() == 0)
    {
        Serial.println("AeroAPIFetcher: No API key configured");
        return false;
    }

    static WiFiClientSecure client;
    if (APIConfiguration::AEROAPI_INSECURE_TLS)
    {
        client.setInsecure();
    }

    // Try up to 2 attempts to handle occasional truncated bodies.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        HTTPClient http;
        String url = String(APIConfiguration::AEROAPI_BASE_URL) + "/flights/" + flightIdent;
        http.begin(client, url);
        http.addHeader("x-apikey", cfg.aeroApiKey);
        http.addHeader("Accept", "application/json");
        http.addHeader("Accept-Encoding", "identity"); // avoid gzip/deflate to reduce parsing issues
        http.addHeader("Connection", "close");          // prefer connection-close to signal body end
        static const char *headers[] = {"Content-Length", "Transfer-Encoding", "Content-Encoding", "Connection"};
        http.collectHeaders(headers, 4);
        http.useHTTP10(true); // prefer non-chunked, connection-close responses
        http.setReuse(false);
        http.setTimeout(30000); // allow longer for full body

        int code = http.GET();
        if (code != 200)
        {
            if (code < 0)
            {
                s_lastTlsFailMs = millis();
            }
            Serial.printf("AeroAPIFetcher: HTTP %d for flight %s -> likely server/network issue\n",
                          code,
                          flightIdent.c_str());
            http.end();
            return false;
        }

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

        int expectedLen = http.getSize();
        String transferEncoding = http.header("Transfer-Encoding");
        String contentEncoding = http.header("Content-Encoding");
        bool isChunked = transferEncoding.equalsIgnoreCase("chunked");

        WiFiClient *stream = http.getStreamPtr();
        if (!stream)
        {
            http.end();
            return false;
        }
        stream->setTimeout(30000);

        static DynamicJsonDocument doc(8192); // reuse to avoid heap churn
        doc.clear();
        DeserializationError err = deserializeJson(doc, *stream, DeserializationOption::Filter(filter));

        if (err)
        {
            Serial.printf("AeroAPIFetcher: JSON parsing failed for flight %s: %s\n",
                          flightIdent.c_str(),
                          err.c_str());
            Serial.printf("AeroAPIFetcher: headers -> content-length=%d transfer-encoding=%s content-encoding=%s chunked=%s\n",
                          expectedLen,
                          transferEncoding.c_str(),
                          contentEncoding.c_str(),
                          isChunked ? "yes" : "no");

            bool truncated = (err == DeserializationError::IncompleteInput);
            http.end();
            if (truncated && attempt == 0)
            {
                Serial.println("AeroAPIFetcher: retrying once due to truncated body");
                delay(200); // brief pause before retry
                continue;
            }
            return false;
        }

        http.end();

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

    // If both attempts failed
    return false;
}
