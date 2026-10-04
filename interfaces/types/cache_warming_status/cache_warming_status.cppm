module;

#include <cstdint>

export module pi.types.cache_warming_status;

import std;
export import pi.types.cache_warming_decision;

/** What the cache warmer is doing. */
export struct CacheWarmingStatus {
    /** "inactive", "scheduled" (a refresh timer is armed) or "refreshing" (a warm request is in flight). */
    std::string state = "inactive";
    /** Why nothing is scheduled. */
    std::optional<std::string> reason;
    std::optional<std::int64_t> nextWarmAt;
    /** The pending decision, or the one that stopped warming. */
    std::optional<CacheWarmingDecision> decision;
    /** A plugin changed `decision.action`. */
    bool extensionOverride = false;
};
