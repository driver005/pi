module;

#include <cstdint>

export module pi.types.cache_previous_request;

import std;

/** The last request a cache scan saw: everything in its prompt should be cached by now. */
export struct CachePreviousRequest {
    std::int64_t promptTokens = 0;
    std::string modelKey;
    std::int64_t timestamp = 0;
    /**
     * Sticky: an earlier request in this scan segment reported cache activity. It tells a total miss on a cache-read-only
     * provider (writes unreported) from a provider that never reports caching.
     */
    bool reportedCache = false;
};
