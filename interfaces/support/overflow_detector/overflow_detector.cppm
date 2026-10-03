module;

#include <cstdint>

export module pi.support.overflow_detector;

import std;
export import pi.types.assistant_message;

/**
 * Recognizes context-window overflow in assistant messages: provider error texts, successful
 * responses whose usage exceeds the window (z.ai) and length stops with no room left (MiMo).
 * Port of utils/overflow.ts.
 */
export class OverflowDetector {
public:
    OverflowDetector()
        : m_overflow(compile({ "prompt (?:is )?too long", "prompt exceeds max length", "request_too_large", "input is too long for requested model", "exceeds the context window", "exceeds (?:the )?(?:model'?s )?maximum context length(?: of [\\d,]+ tokens?|\\s*\\([\\d,]+\\))", "input token count.*exceeds the maximum", "maximum prompt length is \\d+", "reduce the length of the messages", "maximum context length is \\d+ tokens", "exceeds (?:the )?maximum allowed input length of [\\d,]+ tokens?", "input \\(\\d+ tokens\\) is longer than the model'?s context length \\(\\d+ tokens\\)", "exceeds the limit of \\d+", "exceeds the available context size", "greater than the context length", "context window exceeds limit", "exceeded model token limit", "too large for model with \\d+ maximum context length", "prompt has [\\d,]+ tokens?, but the configured context size is [\\d,]+ tokens?", "model_context_window_exceeded", "prompt too long; exceeded (?:max )?context length", "range of input length should be", "context[_ ]length[_ ]exceeded", "too many tokens", "token limit exceeded"})),
          m_nonOverflow(compile({"^(Throttling error|Service unavailable):", "rate limit", "too many requests"})),
          m_cerebrasBodyless("^4(?:00|13)\\s*(?:status code)?\\s*\\(no body\\)", std::regex::ECMAScript | std::regex::icase) {}

    /** contextWindow enables the silent-overflow and length-stop checks; 0 disables them. */
    bool isContextOverflow(const AssistantMessage& message, std::int64_t contextWindow = 0) const {
        if (errorOverflow(message)) {
            return true;
        }
        const std::int64_t inputTokens = message.usage.input + message.usage.cacheRead;
        if (contextWindow > 0 && message.stopReason == StopReason::Stop && inputTokens > contextWindow) {
            return true;
        }
        return contextWindow > 0 && message.stopReason == StopReason::Length && message.usage.output == 0 &&
               static_cast<double>(inputTokens) >= static_cast<double>(contextWindow) * 0.99;
    }

    /** A length stop below the intended output limit; one compact-and-retry may fix it. */
    bool isRecoverableLength(const AssistantMessage& message, std::int64_t desiredMaxOutput) const {
        return message.stopReason == StopReason::Length && desiredMaxOutput > 0 &&
               message.usage.output < desiredMaxOutput;
    }

private:
    std::vector<std::regex> compile(const std::vector<std::string>& patterns) const {
        std::vector<std::regex> out;
        for (const auto& pattern : patterns) {
            out.emplace_back(pattern, std::regex::ECMAScript | std::regex::icase);
        }
        return out;
    }

    bool matchesAny(const std::vector<std::regex>& patterns, const std::string& text) const {
        return std::ranges::any_of(patterns, [&](const std::regex& pattern) {
            return std::regex_search(text, pattern);
        });
    }

    bool errorOverflow(const AssistantMessage& message) const {
        if (message.stopReason != StopReason::Error || !message.errorMessage || message.errorMessage->empty()) {
            return false;
        }
        const std::string& text = *message.errorMessage;
        if (matchesAny(m_nonOverflow, text)) {
            return false;
        }
        return matchesAny(m_overflow, text) ||
               (message.provider == "cerebras" && std::regex_search(text, m_cerebrasBodyless));
    }

    std::vector<std::regex> m_overflow;
    std::vector<std::regex> m_nonOverflow;
    std::regex m_cerebrasBodyless;
};
