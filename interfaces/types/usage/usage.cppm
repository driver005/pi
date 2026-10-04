module;

#include <cstdint>

export module pi.types.usage;

import std;
export import pi.types.usage_cost;

/** Token accounting for one response. `reasoning` is a subset of `output`. */
export struct Usage {
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
