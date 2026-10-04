export module pi.support.message_transformer;

import std;
export import pi.types.assistant_message;
export import pi.types.message;
export import pi.types.model;

/**
 * Rewrites a transcript for a target model: images downgraded for text-only models, thinking
 * and signatures dropped or flattened when the response came from another model, tool call
 * ids renamed for the target API, errored turns removed and orphaned tool calls answered with
 * a synthetic error result. Port of packages/ai/src/api/transform-messages.ts.
 */
export class MessageTransformer {
public:
    /** Maps a foreign tool call id to one the target API accepts. */
    using IdNormalizer =
        std::function<std::string(const std::string&, const Model&, const AssistantMessage&)>;

    /** nowMs stamps synthetic tool results. */
    std::vector<Message> transform(const std::vector<Message>& messages, const Model& model, const IdNormalizer& normalizeToolCallId, std::int64_t nowMs) const {
        return closeOrphans(mapMessages(messages, model, normalizeToolCallId), nowMs);
    }

    /** Replaces tool call ids with `[a-zA-Z0-9_-]` only, at most `maxLength` characters. */
    std::string sanitizeToolCallId(const std::string& id, std::size_t maxLength) const {
        std::string out;
        for (const char c : id) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '-';
            out.push_back(ok ? c : '_');
        }
        if (out.size() > maxLength) {
            out.resize(maxLength);
        }
        return out;
    }

private:
    std::vector<UserContentBlock> replaceImages(const std::vector<UserContentBlock>& content, const std::string& placeholder) const {
        std::vector<UserContentBlock> result;
        bool previousWasPlaceholder = false;
        for (const auto& block : content) {
            if (std::holds_alternative<ImageContent>(block)) {
                if (!previousWasPlaceholder) {
                    result.emplace_back(TextContent{placeholder, std::nullopt});
                }
                previousWasPlaceholder = true;
                continue;
            }
            result.push_back(block);
            previousWasPlaceholder = std::get<TextContent>(block).text == placeholder;
        }
        return result;
    }

    std::vector<Message> downgradeImages(const std::vector<Message>& messages, const Model& model) const {
        if (std::find(model.input.begin(), model.input.end(), "image") != model.input.end()) {
            return messages;
        }
        std::vector<Message> result = messages;
        for (auto& message : result) {
            if (auto* user = std::get_if<UserMessage>(&message)) {
                if (auto* blocks = std::get_if<std::vector<UserContentBlock>>(&user->content)) {
                    *blocks = replaceImages(*blocks, "(image omitted: model does not support images)");
                }
            } else if (auto* toolResult = std::get_if<ToolResultMessage>(&message)) {
                toolResult->content = replaceImages(
                    toolResult->content, "(tool image omitted: model does not support images)");
            }
        }
        return result;
    }

    bool isSameModel(const AssistantMessage& message, const Model& model) const {
        return message.provider == model.provider && message.api == model.api &&
               message.model == model.id;
    }

    AssistantMessage convertAssistant(const AssistantMessage& message, const Model& model, const IdNormalizer& normalizeToolCallId, std::map<std::string, std::string>& idMap) const {
        const bool same = isSameModel(message, model);
        AssistantMessage out = message;
        out.content.clear();
        for (const auto& block : message.content) {
            if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
                if (thinking->redacted.value_or(false)) {
                    if (same) {
                        out.content.push_back(block);
                    }
                    continue;
                }
                if (same && thinking->thinkingSignature && !thinking->thinkingSignature->empty()) {
                    out.content.push_back(block);
                    continue;
                }
                const bool blank = thinking->thinking.find_first_not_of(" \t\r\n") == std::string::npos;
                if (blank) {
                    continue;
                }
                if (same) {
                    out.content.push_back(block);
                } else {
                    out.content.emplace_back(TextContent{thinking->thinking, std::nullopt});
                }
            } else if (const auto* text = std::get_if<TextContent>(&block)) {
                if (same) {
                    out.content.push_back(block);
                } else {
                    out.content.emplace_back(TextContent{text->text, std::nullopt});
                }
            } else {
                ToolCall call = std::get<ToolCall>(block);
                if (!same) {
                    call.thoughtSignature.reset();
                    if (normalizeToolCallId) {
                        std::string normalized = normalizeToolCallId(call.id, model, message);
                        if (normalized != call.id) {
                            idMap[call.id] = normalized;
                            call.id = std::move(normalized);
                        }
                    }
                }
                out.content.emplace_back(std::move(call));
            }
        }
        return out;
    }

    std::vector<Message> mapMessages(const std::vector<Message>& messages, const Model& model, const IdNormalizer& normalizeToolCallId) const {
        std::map<std::string, std::string> idMap;
        std::vector<Message> result;
        result.reserve(messages.size());
        for (const auto& message : downgradeImages(messages, model)) {
            if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
                result.emplace_back(convertAssistant(*assistant, model, normalizeToolCallId, idMap));
            } else if (const auto* toolResult = std::get_if<ToolResultMessage>(&message)) {
                ToolResultMessage copy = *toolResult;
                auto found = idMap.find(copy.toolCallId);
                if (found != idMap.end()) {
                    copy.toolCallId = found->second;
                }
                result.emplace_back(std::move(copy));
            } else {
                result.push_back(message);
            }
        }
        return result;
    }

    std::vector<Message> closeOrphans(const std::vector<Message>& messages, std::int64_t nowMs) const {
        std::vector<Message> result;
        std::vector<ToolCall> pending;
        std::set<std::string> answered;
        std::vector<Message> held;

        auto close = [&]() {
            for (const auto& call : pending) {
                if (answered.contains(call.id)) {
                    continue;
                }
                ToolResultMessage synthetic;
                synthetic.toolCallId = call.id;
                synthetic.toolName = call.name;
                synthetic.content.emplace_back(TextContent{"No result provided", std::nullopt});
                synthetic.isError = true;
                synthetic.timestamp = nowMs;
                result.emplace_back(std::move(synthetic));
            }
            pending.clear();
            answered.clear();
            result.insert(result.end(), held.begin(), held.end());
            held.clear();
        };

        for (const auto& message : messages) {
            if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
                close();
                if (assistant->stopReason == StopReason::Error ||
                    assistant->stopReason == StopReason::Aborted) {
                    continue;
                }
                for (const auto& block : assistant->content) {
                    if (const auto* call = std::get_if<ToolCall>(&block)) {
                        pending.push_back(*call);
                    }
                }
                result.push_back(message);
            } else if (const auto* toolResult = std::get_if<ToolResultMessage>(&message)) {
                answered.insert(toolResult->toolCallId);
                result.push_back(message);
            } else if (std::holds_alternative<SystemMessage>(message)) {
                if (pending.empty()) {
                    result.push_back(message);
                } else {
                    held.push_back(message);
                }
            } else {
                close();
                result.push_back(message);
            }
        }
        close();
        return result;
    }
};
