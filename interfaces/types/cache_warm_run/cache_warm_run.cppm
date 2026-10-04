module;

#include <cstdint>

export module pi.types.cache_warm_run;

import std;
export import pi.support.abort_signal;
export import pi.types.cache_warm_request;

/** One prompt-cache entry the warmer keeps alive, with its timing. */
export struct CacheWarmRun {
    CacheWarmRequest request;
    /** False once the session's model or messages no longer match the request. */
    std::function<bool()> isCurrent;
    std::int64_t ttlMs = 0;
    std::int64_t delayMs = 0;
    /** Latest safe time to send this refresh, leaving half the original expiry margin. */
    std::int64_t refreshDeadlineAt = 0;
    std::int64_t startedAt = 0;
    std::shared_ptr<AbortSignal> abort = std::make_shared<AbortSignal>();
    /** "streaming" or "idle". */
    std::string phase = "streaming";
    std::int64_t nextWarmAt = 0;
    /** A refresh a plugin forced is in flight. */
    bool extensionOverride = false;
    /** A timer thread is waiting to refresh. */
    bool timerArmed = false;
};
