export module pi.support.assistant_error_classifier;

import std;
export import pi.types.assistant_message;

/**
 * Tells transient provider and transport failures (rate limits, overload, dropped streams) from
 * deterministic ones (quota exhausted, bad request) by the error text of a failed assistant
 * message. Port of isRetryableAssistantError in utils/retry.ts.
 */
export class AssistantErrorClassifier {
public:
    AssistantErrorClassifier()
        : m_nonRetryable(build({ "GoUsageLimitError", "FreeUsageLimitError", "Monthly usage limit reached", "available balance", "insufficient_quota", "out of budget", "quota exceeded", "billing", "subscription_sharing_usage_limit_exceeded"})),
          m_retryable(build({ "overloaded", "currently experiencing high demand", "model is at capacity", "rate.?limit", "too many requests", "429", "500", "502", "503", "504", "520", "524", "service.?unavailable", "server.?error", "internal.?error", "provider.?returned.?error", "exceeded request buffer limit while retrying upstream", "network.?error", "connection.?error", "connection.?refused", "connection.?lost", "other side closed", "fetch failed", "getaddrinfo", "ENOTFOUND", "EAI_AGAIN", "upstream.?connect", "reset before headers", "socket hang up", "socket connection was closed", "timed? out", "timeout", "terminated", "websocket.?closed", "websocket.?error", "ended without", "stream ended before message_stop", "stream ended before a terminal response event", "http2 request did not get a response", "retry delay", "you can retry your request", "try your request again", "please retry your request", "ResourceExhausted", "subscription_sharing_usage_unavailable", "subscription_sharing_user_unavailable"})) {}

    bool isRetryable(const AssistantMessage& message) const {
        if (message.stopReason != StopReason::Error || !message.errorMessage || message.errorMessage->empty()) {
            return false;
        }
        const std::string& text = *message.errorMessage;
        if (std::regex_search(text, m_nonRetryable)) {
            return false;
        }
        return std::regex_search(text, m_retryable);
    }

private:
    std::regex build(const std::vector<std::string>& patterns) const {
        std::string joined;
        for (std::size_t i = 0; i < patterns.size(); ++i) {
            joined += (i == 0 ? "" : "|") + patterns[i];
        }
        return std::regex(joined, std::regex::ECMAScript | std::regex::icase);
    }

    std::regex m_nonRetryable;
    std::regex m_retryable;
};
