export module pi.support.token_estimator;

import std;
export import pi.support.transcript_normalizer;
export import pi.types.context_usage_estimate;
export import pi.types.message;
export import pi.types.transcript_context;
export import pi.types.usage;

/** Character-based token estimates (4 chars per token). Port of utils/estimate.ts. */
export class TokenEstimator {
public:
    std::int64_t contextTokens(const Usage& usage) const {
        return usage.totalTokens != 0
                   ? usage.totalTokens
                   : usage.input + usage.output + usage.cacheRead + usage.cacheWrite;
    }

    std::int64_t textTokens(const std::string& text) const {
        return charsToTokens(text.size());
    }

    std::int64_t messageTokens(const Message& message) const {
        if (const auto* system = std::get_if<SystemMessage>(&message)) {
            std::int64_t tokens = textTokens(m_normalizer.systemMessageText(*system));
            tokens += toolsTokens(system->toolsAdded);
            if (system->toolsRemoved && !system->toolsRemoved->empty()) {
                tokens += static_cast<std::int64_t>(system->toolsRemoved->size());
            }
            return tokens;
        }
        if (const auto* user = std::get_if<UserMessage>(&message)) {
            if (const auto* text = std::get_if<std::string>(&user->content)) {
                return charsToTokens(text->size());
            }
            return charsToTokens(blockChars(std::get<std::vector<UserContentBlock>>(user->content)));
        }
        if (const auto* result = std::get_if<ToolResultMessage>(&message)) {
            return charsToTokens(blockChars(result->content));
        }
        return assistantTokens(std::get<AssistantMessage>(message));
    }

    ContextUsageEstimate estimate(const std::vector<Message>& messages) const {
        ContextUsageEstimate result;
        const int index = lastUsageIndex(messages);
        if (index >= 0) {
            result.usageTokens = contextTokens(std::get<AssistantMessage>(messages[static_cast<std::size_t>(index)]).usage);
            for (std::size_t i = static_cast<std::size_t>(index) + 1; i < messages.size(); ++i) {
                result.trailingTokens += messageTokens(messages[i]);
            }
            result.tokens = result.usageTokens + result.trailingTokens;
            result.lastUsageIndex = index;
            return result;
        }
        for (const auto& message : messages) {
            result.tokens += messageTokens(message);
        }
        result.trailingTokens = result.tokens;
        return result;
    }

    ContextUsageEstimate estimate(const TranscriptContext& context) const {
        return estimate(context.messages);
    }

private:
    std::int64_t charsToTokens(std::size_t chars) const {
        return static_cast<std::int64_t>((chars + 3) / 4);
    }

    std::size_t blockChars(const std::vector<UserContentBlock>& blocks) const {
        std::size_t chars = 0;
        for (const auto& block : blocks) {
            chars += std::holds_alternative<TextContent>(block) ? std::get<TextContent>(block).text.size()
                                                                : 4800;
        }
        return chars;
    }

    std::int64_t toolsTokens(const std::optional<std::vector<Tool>>& tools) const {
        if (!tools || tools->empty()) {
            return 0;
        }
        Json array = Json::array();
        for (const auto& tool : *tools) {
            Json item = Json::object();
            item["name"] = tool.name;
            item["description"] = tool.description;
            item["parameters"] = tool.parameters;
            array.push_back(std::move(item));
        }
        return textTokens(array.dump(-1, ' ', false, Json::error_handler_t::replace));
    }

    std::int64_t assistantTokens(const AssistantMessage& message) const {
        std::size_t chars = 0;
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<TextContent>(&block)) {
                chars += text->text.size();
            } else if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
                chars += thinking->thinking.size();
            } else {
                const auto& call = std::get<ToolCall>(block);
                chars += call.name.size() +
                         call.arguments.dump(-1, ' ', false, Json::error_handler_t::replace).size();
            }
        }
        return charsToTokens(chars);
    }

    int lastUsageIndex(const std::vector<Message>& messages) const {
        std::int64_t latestPrefixTimestamp = std::numeric_limits<std::int64_t>::min();
        int found = -1;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            std::int64_t timestamp = 0;
            std::visit([&](const auto& message) { timestamp = message.timestamp; }, messages[i]);
            if (const auto* assistant = std::get_if<AssistantMessage>(&messages[i])) {
                if (assistant->timestamp >= latestPrefixTimestamp &&
                    assistant->stopReason != StopReason::Aborted &&
                    assistant->stopReason != StopReason::Error &&
                    contextTokens(assistant->usage) > 0) {
                    found = static_cast<int>(i);
                }
            }
            latestPrefixTimestamp = std::max(latestPrefixTimestamp, timestamp);
        }
        return found;
    }

    TranscriptNormalizer m_normalizer;
};
