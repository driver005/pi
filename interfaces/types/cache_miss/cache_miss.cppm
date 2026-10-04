module;

#include <cstdint>

export module pi.types.cache_miss;

import std;

/** A counted prompt-cache miss on one assistant message. */
export struct CacheMiss {
    /** Prompt tokens that were in the previous turn's prompt but not read from cache. */
    std::int64_t missedTokens = 0;
    /** Extra dollars paid versus a full cache hit; 0 when pricing is unknown. */
    double missedCost = 0;
    /** Milliseconds since the previous request, which last refreshed the cache. */
    std::int64_t idleMs = 0;
    /** The model changed relative to the previous request. */
    bool modelChanged = false;
};
