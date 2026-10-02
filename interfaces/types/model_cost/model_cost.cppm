export module pi.types.model_cost;

import std;
export import pi.types.model_cost_tier;

/** Base prices in $ per million tokens plus optional request-wide tiers (highest match wins). */
export struct ModelCost {
    double input = 0;
    double output = 0;
    double cacheRead = 0;
    double cacheWrite = 0;
    std::vector<ModelCostTier> tiers;
};
