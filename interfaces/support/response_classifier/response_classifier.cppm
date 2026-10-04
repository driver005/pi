module;

#include <cstdint>

export module pi.support.response_classifier;

import std;
export import pi.support.assistant_error_classifier;
export import pi.support.message_codec;
export import pi.support.overflow_detector;
export import pi.types.conversation_retry_policy;
export import pi.types.json;

/**
 * Classifies a provider's final assistant message (JSON) for the durable harness: whether its error is transient, whether
 * it is a context overflow, and how long to back off before retrying attempt N.
 */
export class ResponseClassifier {
public:
    /** Whether the message ended in an error that looks transient. */
    bool retryable(const Json& message) const {
        auto typed = m_codec.assistantMessageFromJson(message);
        return typed && m_errors.isRetryable(*typed);
    }

    /** Whether the message is the provider's context overflow error. */
    bool contextOverflow(const Json& message, std::int64_t contextWindow = 0) const {
        auto typed = m_codec.assistantMessageFromJson(message);
        return typed && m_overflow.isContextOverflow(*typed, contextWindow);
    }

    /** `baseDelayMs * 2^(attempt - 1)`, capped at `maxAgentDelayMs` (default 60 s). */
    std::int64_t retryDelayMs(const ConversationRetryPolicy& policy, std::int64_t attempt) const {
        const std::int64_t cap = policy.maxAgentDelayMs.value_or(kDefaultMaxDelayMs);
        std::int64_t delay = policy.baseDelayMs;
        for (std::int64_t step = 1; step < attempt && delay < cap; ++step) {
            delay = delay > std::numeric_limits<std::int64_t>::max() / 2 ? std::numeric_limits<std::int64_t>::max() : delay * 2;
        }
        return std::min(delay, cap);
    }

private:
    static constexpr std::int64_t kDefaultMaxDelayMs = 60000;

    MessageCodec m_codec;
    AssistantErrorClassifier m_errors;
    OverflowDetector m_overflow;
};
