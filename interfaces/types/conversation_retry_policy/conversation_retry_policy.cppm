module;

#include <cstdint>

export module pi.types.conversation_retry_policy;

import std;

/** Durable generation attempt retries. */
export struct ConversationRetryPolicy {
    bool enabled = true;
    std::int64_t maxRetries = 3;
    std::int64_t baseDelayMs = 2000;
    std::optional<std::int64_t> maxAgentDelayMs = 60000;
};
