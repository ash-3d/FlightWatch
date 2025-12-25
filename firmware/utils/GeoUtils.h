#pragma once

#include <math.h>

constexpr double kPi = 3.14159265358979323846;
inline double degreesToRadians(double deg) { return deg * kPi / 180.0; }
inline double radiansToDegrees(double rad) { return rad * 180.0 / kPi; }

inline double haversineKm(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371.0; // Earth radius in km
    const double dlat = degreesToRadians(lat2 - lat1);
    const double dlon = degreesToRadians(lon2 - lon1);
    const double a = sin(dlat / 2) * sin(dlat / 2) +
                     cos(degreesToRadians(lat1)) * cos(degreesToRadians(lat2)) *
                         sin(dlon / 2) * sin(dlon / 2);
    const double c = 2 * asin(sqrt(a));
    return R * c;
}

inline double computeBearingDeg(double lat1, double lon1, double lat2, double lon2)
{
    const double dlon = degreesToRadians(lon2 - lon1);
    const double lat1r = degreesToRadians(lat1);
    const double lat2r = degreesToRadians(lat2);
    const double x = sin(dlon) * cos(lat2r);
    const double y = cos(lat1r) * sin(lat2r) - sin(lat1r) * cos(lat2r) * cos(dlon);
    const double initial = atan2(x, y);
    const double deg = fmod((radiansToDegrees(initial) + 360.0), 360.0);
    return deg;
}

inline void centeredBoundingBox(double lat, double lon, double radiusKm,
                                double &latMin, double &latMax,
                                double &lonMin, double &lonMax)
{
    const double latDelta = radiusKm / 111.0;
    const double lonDelta = radiusKm / (111.0 * cos(degreesToRadians(lat)));
    latMin = lat - latDelta;
    latMax = lat + latDelta;
    lonMin = lon - lonDelta;
    lonMax = lon + lonDelta;
}
