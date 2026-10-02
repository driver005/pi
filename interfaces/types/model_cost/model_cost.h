#pragma once

#include <vector>

#include "interfaces/types/model_cost_tier/model_cost_tier.h"

/** Base prices in $ per million tokens plus optional request-wide tiers (highest match wins). */
struct ModelCost {
    double input = 0;
    double output = 0;
    double cacheRead = 0;
    double cacheWrite = 0;
    std::vector<ModelCostTier> tiers;
};
