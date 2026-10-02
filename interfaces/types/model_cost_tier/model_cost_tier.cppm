module;

#include <cstdint>

export module pi.types.model_cost_tier;

import std;

/** Pricing ($ per million tokens) that applies when a request's input exceeds a threshold. */
export struct ModelCostTier {
    std::int64_t inputTokensAbove = 0;
    double input = 0;
    double output = 0;
    double cacheRead = 0;
    double cacheWrite = 0;
};
