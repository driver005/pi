module;

#include <cstdint>

export module pi.types.cache_waste_totals;

import std;

/** Prompt-cache waste summed over a session. */
export struct CacheWasteTotals {
    std::int64_t missedTokens = 0;
    double missedCost = 0;
    /** Counted misses: turns above the noise floor. */
    std::int64_t missCount = 0;
};
