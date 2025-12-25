#pragma once

#include <vector>
#include "models/StateVector.h"

class BaseStateVectorFetcher
{
public:
    virtual ~BaseStateVectorFetcher() = default;

    virtual bool fetchStateVectors(
        double centerLat,
        double centerLon,
        double radiusKm,
        std::vector<StateVector> &outStateVectors) = 0;
};
