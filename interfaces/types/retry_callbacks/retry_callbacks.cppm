module;

#include <cstdint>

export module pi.types.retry_callbacks;

import std;

/** Optional observers of a retried assistant call; unset members are skipped. */
export struct RetryCallbacks {
    /** Before the backoff sleep of each retry (attempt is 1-based). */
    std::function<void(int attempt, int maxAttempts, std::int64_t delayMs, const std::string& error)>
        onRetryScheduled;
    /** After the backoff sleep, just before the retried call starts. */
    std::function<void()> onRetryAttemptStart;
    /** Once when the loop ends; success is true when a later call completed normally. */
    std::function<void(bool success, int attempt, const std::optional<std::string>& finalError)>
        onRetryFinished;
};
