/*
Purpose: Orchestrate fetching and enrichment of flight data for display.
Flow:
1) Use BaseStateVectorFetcher to fetch nearby state vectors by geo filter.
2) For each callsign, use BaseFlightFetcher (e.g., AeroAPI) to retrieve FlightInfo.
3) Enrich names using AeroAPI data when present, with embedded lookup tables (no CDN dependency).
Output: Returns count of enriched flights and fills outStates/outFlights.
*/
#include "core/FlightDataFetcher.h"
#include "config/RuntimeSettings.h"
#include <strings.h>

struct LookupEntry { const char *icao; const char *name; };

// Generated full tables (airlines/aircraft) live here; regenerate via tools/generate_lookup_header.py.
#include "LookupTables.generated.h"

static String lookupFromTable(const LookupEntry *table, size_t count, const String &icao)
{
    if (icao.length() == 0 || count == 0)
        return String("");

    int low = 0;
    int high = static_cast<int>(count) - 1;
    while (low <= high)
    {
        int mid = low + ((high - low) / 2);
        int cmp = strcasecmp(icao.c_str(), table[mid].icao);
        if (cmp == 0)
        {
            return String(table[mid].name);
        }
        if (cmp < 0)
        {
            high = mid - 1;
        }
        else
        {
            low = mid + 1;
        }
    }
    return String("");
}

static String normalizeAircraftLabel(String label)
{
    label.trim();
    label.replace("Freighter", "");
    label.replace("freighter", "");
    label.replace("FREIGHTER", "");
    label.replace("pax", "");
    label.replace("PAX", "");
    label.trim();
    // Collapse double spaces that might remain after removals.
    while (label.indexOf("  ") >= 0)
    {
        label.replace("  ", " ");
    }
    label.trim();
    const size_t MAX_LEN = 10;
    if (label.length() > MAX_LEN)
    {
        label = label.substring(0, MAX_LEN);
    }
    return label;
}

static String deriveAirlineFromCallsign(const String &callsign)
{
    String cs = callsign;
    cs.trim();
    // Take leading letters (strip digits/suffix). Most ICAO prefixes are 3 letters; some IATA are 2.
    String prefix;
    for (size_t i = 0; i < cs.length(); ++i)
    {
        char c = cs[i];
        if (isalpha(static_cast<unsigned char>(c)))
            prefix += static_cast<char>(toupper(c));
        else
            break;
    }
    if (prefix.length() >= 3)
    {
        return prefix.substring(0, 3);
    }
    if (prefix.length() == 2)
    {
        return prefix;
    }
    return String("");
}

FlightDataFetcher::FlightDataFetcher(BaseStateVectorFetcher *stateFetcher,
                                     BaseFlightFetcher *flightFetcher)
    : _stateFetcher(stateFetcher), _flightFetcher(flightFetcher) {}

size_t FlightDataFetcher::fetchFlights(std::vector<StateVector> &outStates,
                                       std::vector<FlightInfo> &outFlights)
{
    outStates.clear();
    outFlights.clear();

    const auto &cfg = RuntimeSettings::current();
    bool ok = _stateFetcher->fetchStateVectors(
        cfg.centerLat,
        cfg.centerLon,
        cfg.radiusKm,
        outStates);
    if (!ok)
        return 0;

    size_t enriched = 0;
    for (const StateVector &s : outStates)
    {
        if (s.callsign.length() == 0)
        {
            continue;
        }
        FlightInfo info;
        if (_flightFetcher->fetchFlightInfo(s.callsign, info))
        {
            // Carry forward live metrics from the state vector
            info.baro_altitude_m = s.baro_altitude;
            info.velocity_mps = s.velocity;

            // Prefer AeroAPI operator_icao mapped to full name; fall back to operator_code; then callsign-derived prefix.
            if (info.operator_icao.length())
            {
                String opIcao = info.operator_icao;
                opIcao.trim();
                String airline = lookupFromTable(kAirlineLookup, kAirlineLookup_COUNT, opIcao);
                if (airline.length() == 0)
                {
                    airline = opIcao; // last-resort code for readability
                }
                info.airline_display_name_full = airline;
            }
            else if (info.operator_code.length())
            {
                info.airline_display_name_full = info.operator_code;
            }
            else
            {
                // Derive from callsign prefix if AeroAPI returned nothing.
                String prefix = deriveAirlineFromCallsign(s.callsign);
                if (prefix.length())
                {
                    String airline = lookupFromTable(kAirlineLookup, kAirlineLookup_COUNT, prefix);
                    info.airline_display_name_full = airline.length() ? airline : prefix;
                }
                else
                {
                    // Debug: AeroAPI returned no operator info; log once for visibility.
                    static int missingOpLogCount = 0;
                    if (missingOpLogCount < 5)
                    {
                        Serial.printf("Enrichment: missing operator for ident=%s\n", s.callsign.c_str());
                        missingOpLogCount++;
                    }
                }
            }

            if (info.aircraft_code.length())
            {
                String acIcao = info.aircraft_code;
                acIcao.trim();
                String aircraftShort = lookupFromTable(kAircraftLookup, kAircraftLookup_COUNT, acIcao);
                if (aircraftShort.length() == 0)
                {
                    aircraftShort = acIcao; // last-resort code
                }
                aircraftShort = normalizeAircraftLabel(aircraftShort);
                if (aircraftShort.length() == 0)
                {
                    aircraftShort = acIcao; // ensure non-empty label
                }
                info.aircraft_display_name_short = aircraftShort;
            }
            outFlights.push_back(info);
            enriched++;
        }
    }
    return enriched;
}
