#pragma once

#include <cstdint>
#include <optional>

#include "interfaces/types/usage_cost/usage_cost.h"

/** Token accounting for one response. `reasoning` is a subset of `output`. */
struct Usage {
    std::int64_t input = 0;
    std::int64_t output = 0;
    std::int64_t cacheRead = 0;
    std::int64_t cacheWrite = 0;
    /** Subset of cacheWrite written with 1h retention (Anthropic only). */
    std::optional<std::int64_t> cacheWrite1h;
    std::optional<std::int64_t> reasoning;
    std::int64_t totalTokens = 0;
    UsageCost cost;
};
