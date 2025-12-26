#include "config/RuntimeSettings.h"
#include "config/IanaPosixDb.h"
#include <pgmspace.h>

static FlightWatchSettings g_settings;
static const char *const NVS_NAMESPACE = "fwsettings";

static String resolvePosixFromIana(const String &iana, const String &fallback)
{
    String key = iana;
    key.toLowerCase();
    for (size_t i = 0; i < IANA_POSIX_DB_COUNT; ++i)
    {
        if (key.equals(IANA_POSIX_DB[i].iana))
        {
            return String(IANA_POSIX_DB[i].posix);
        }
    }
    if (fallback.length())
        return fallback;
    return String("UTC0");
}

static double stringToDouble(const String &s, double fallback)
{
    if (s.length() == 0)
        return fallback;
    char *endptr = nullptr;
    double v = strtod(s.c_str(), &endptr);
    if (endptr == s.c_str())
        return fallback;
    return v;
}

void RuntimeSettings::load()
{
    // Defaults from compile-time configuration
    g_settings.centerLat = UserConfiguration::CENTER_LAT;
    g_settings.centerLon = UserConfiguration::CENTER_LON;
    g_settings.radiusKm  = UserConfiguration::RADIUS_KM;

    g_settings.weatherLat = UserConfiguration::CENTER_LAT;
    g_settings.weatherLon = UserConfiguration::CENTER_LON;

    g_settings.displayBrightness = UserConfiguration::DISPLAY_BRIGHTNESS;
    g_settings.textColorR = UserConfiguration::TEXT_COLOR_R;
    g_settings.textColorG = UserConfiguration::TEXT_COLOR_G;
    g_settings.textColorB = UserConfiguration::TEXT_COLOR_B;
    g_settings.altitudeFeet = UserConfiguration::ALTITUDE_FEET;
    g_settings.speedKts = UserConfiguration::SPEED_KTS;

    g_settings.timezoneIana = UserConfiguration::TIMEZONE_IANA;
    g_settings.timezonePosix = resolvePosixFromIana(g_settings.timezoneIana, UserConfiguration::TIMEZONE_TZ);

    g_settings.aeroApiKey = APIConfiguration::AEROAPI_KEY;
    g_settings.openSkyClientId = APIConfiguration::OPENSKY_CLIENT_ID;
    g_settings.openSkyClientSecret = APIConfiguration::OPENSKY_CLIENT_SECRET;

    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true))
    {
        return;
    }

    g_settings.centerLat = prefs.getDouble("centerLat", g_settings.centerLat);
    g_settings.centerLon = prefs.getDouble("centerLon", g_settings.centerLon);
    g_settings.radiusKm  = prefs.getDouble("radiusKm", g_settings.radiusKm);

    g_settings.weatherLat = prefs.getDouble("weatherLat", g_settings.weatherLat);
    g_settings.weatherLon = prefs.getDouble("weatherLon", g_settings.weatherLon);

    g_settings.displayBrightness = prefs.getUInt("dispBright", g_settings.displayBrightness);
    g_settings.textColorR = prefs.getUInt("colorR", g_settings.textColorR);
    g_settings.textColorG = prefs.getUInt("colorG", g_settings.textColorG);
    g_settings.textColorB = prefs.getUInt("colorB", g_settings.textColorB);
    g_settings.altitudeFeet = prefs.getBool("altFeet", g_settings.altitudeFeet);
    g_settings.speedKts = prefs.getBool("spdKts", g_settings.speedKts);

    g_settings.timezoneIana = prefs.getString("tzIana", g_settings.timezoneIana);
    g_settings.timezonePosix = resolvePosixFromIana(g_settings.timezoneIana, g_settings.timezonePosix);

    g_settings.aeroApiKey = prefs.getString("aeroKey", g_settings.aeroApiKey);
    g_settings.openSkyClientId = prefs.getString("osId", g_settings.openSkyClientId);
    g_settings.openSkyClientSecret = prefs.getString("osSecret", g_settings.openSkyClientSecret);

    prefs.end();
}

bool RuntimeSettings::save(const FlightWatchSettings &newSettings)
{
    FlightWatchSettings copy = newSettings;
    copy.timezonePosix = resolvePosixFromIana(copy.timezoneIana, copy.timezonePosix);

    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false))
    {
        return false;
    }

    prefs.putDouble("centerLat", copy.centerLat);
    prefs.putDouble("centerLon", copy.centerLon);
    prefs.putDouble("radiusKm", copy.radiusKm);

    prefs.putDouble("weatherLat", copy.weatherLat);
    prefs.putDouble("weatherLon", copy.weatherLon);

    prefs.putUInt("dispBright", copy.displayBrightness);
    prefs.putUInt("colorR", copy.textColorR);
    prefs.putUInt("colorG", copy.textColorG);
    prefs.putUInt("colorB", copy.textColorB);
    prefs.putBool("altFeet", copy.altitudeFeet);
    prefs.putBool("spdKts", copy.speedKts);

    prefs.putString("tzIana", copy.timezoneIana);
    prefs.putString("tzPosix", copy.timezonePosix);
    prefs.putString("aeroKey", copy.aeroApiKey);
    prefs.putString("osId", copy.openSkyClientId);
    prefs.putString("osSecret", copy.openSkyClientSecret);

    prefs.end();

    g_settings = copy;
    return true;
}

const FlightWatchSettings &RuntimeSettings::current()
{
    return g_settings;
}
