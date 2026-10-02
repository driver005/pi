module;

#include <cstdint>

export module pi.types.assistant_retry_policy;

import std;

/** Bounded retry of a failed assistant call (settings.retry); delay doubles per attempt. */
export struct AssistantRetryPolicy {
    bool enabled = false;
    /** Retries after the initial call; 0 means none. */
    int maxRetries = 0;
    std::int64_t baseDelayMs = 2000;
    /** Cap for each computed delay. */
    std::int64_t maxDelayMs = 60000;
};
