#pragma once

#include <cstdint>

/** Pricing ($ per million tokens) that applies when a request's input exceeds a threshold. */
struct ModelCostTier {
    std::int64_t inputTokensAbove = 0;
    double input = 0;
    double output = 0;
    double cacheRead = 0;
    double cacheWrite = 0;
};
