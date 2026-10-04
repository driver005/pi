module;

#include <cstdint>

export module pi.support.mistral_request_builder;

import std;
export import pi.support.message_transformer;
export import pi.support.short_hash;
export import pi.support.thinking_budget_calculator;
export import pi.support.thinking_level_resolver;
export import pi.support.transcript_normalizer;
export import pi.types.json;
export import pi.types.model;
export import pi.types.stream_options;
export import pi.types.transcript_context;

/**
 * Builds Mistral chat completion request bodies in the native wire format (snake_case). Port of
 * buildChatPayload, toChatMessages and the reasoning option mapping in
 * api/mistral-conversations.ts. Strict tool schemas are not ported.
 */
export class MistralRequestBuilder {
public:
    static constexpr std::size_t ToolCallIdLength = 9;

    Json build(const Model& model, const TranscriptContext& rawContext, const StreamOptions& options, std::int64_t nowMs) const {
        const bool midConvo = model.compat.is_object() && model.compat.value("supportsMidConvoSystemMessages", false);
        const TranscriptContext context = m_normalizer.resolveTranscript(rawContext, midConvo);
        std::map<std::string, std::string> assigned;
        std::map<std::string, std::string> owners;
        const auto normalizeId = [&](const std::string& id, const Model&, const AssistantMessage&) {
            const auto known = assigned.find(id);
            if (known != assigned.end()) {
                return known->second;
            }
            for (int attempt = 0;; ++attempt) {
                const std::string candidate = deriveToolCallId(id, attempt);
                const auto owner = owners.find(candidate);
                if (owner == owners.end() || owner->second == id) {
                    assigned[id] = candidate;
                    owners[candidate] = id;
                    return candidate;
                }
            }
        };
        const auto transformed = m_transformer.transform(context.messages, model, normalizeId, nowMs);
        Json payload = Json::object();
        payload["model"] = model.id;
        payload["stream"] = true;
        payload["messages"] = convertMessages(model, transformed);
        const std::vector<Tool> tools = m_normalizer.currentTools(context.messages);
        if (!tools.empty()) {
            payload["tools"] = convertTools(tools);
        }
        if (options.temperature) {
            payload["temperature"] = *options.temperature;
        }
        const std::int64_t maxTokens =
            m_budgets.clampMaxTokensToContext(model, context, options.maxTokens.value_or(model.maxTokens));
        if (maxTokens > 0) {
            payload["max_tokens"] = maxTokens;
        }
        if (options.toolChoice) {
            payload["tool_choice"] = *options.toolChoice;
        }
        applyReasoning(payload, model, options);
        if (usesPromptCaching(options)) {
            payload["prompt_cache_key"] = *options.sessionId;
        }
        return payload;
    }

    /** Whether the request carries the prompt cache key (cache not disabled and a session id). */
    bool usesPromptCaching(const StreamOptions& options) const {
        return options.cacheRetention.value_or("short") != "none" && options.sessionId && !options.sessionId->empty();
    }

private:
    std::string deriveToolCallId(const std::string& id, int attempt) const {
        std::string normalized;
        for (const char c : id) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
                normalized.push_back(c);
            }
        }
        if (attempt == 0 && normalized.size() == ToolCallIdLength) {
            return normalized;
        }
        const std::string base = normalized.empty() ? id : normalized;
        const std::string seed = attempt == 0 ? base : base + ":" + std::to_string(attempt);
        std::string hashed;
        for (const char c : m_hash.of(seed)) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
                hashed.push_back(c);
            }
        }
        return hashed.substr(0, ToolCallIdLength);
    }

    bool isBlank(const std::string& text) const {
        return text.find_first_not_of(" \t\r\n") == std::string::npos;
    }

    Json textChunk(const std::string& text) const {
        return Json{{"type", "text"}, {"text", text}};
    }

    Json imageChunk(const ImageContent& image) const {
        return Json{{"type", "image_url"}, {"image_url", "data:" + image.mimeType + ";base64," + image.data}};
    }

    Json systemMessage(const Model&, const SystemMessage& message, bool leading) const {
        const std::string text = leading ? m_normalizer.systemMessageText(message)
                                         : m_normalizer.renderSystemMessageUpdate(message);
        return text.empty() ? Json() : Json{{"role", "system"}, {"content", text}};
    }

    void appendUser(const UserMessage& message, bool supportsImages, Json& out) const {
        if (const auto* text = std::get_if<std::string>(&message.content)) {
            out.push_back(Json{{"role", "user"}, {"content", *text}});
            return;
        }
        Json content = Json::array();
        bool hadImages = false;
        for (const auto& block : std::get<std::vector<UserContentBlock>>(message.content)) {
            if (const auto* text = std::get_if<TextContent>(&block)) {
                content.push_back(textChunk(text->text));
            } else {
                hadImages = true;
                if (supportsImages) {
                    content.push_back(imageChunk(std::get<ImageContent>(block)));
                }
            }
        }
        if (!content.empty()) {
            out.push_back(Json{{"role", "user"}, {"content", std::move(content)}});
        } else if (hadImages && !supportsImages) {
            out.push_back(Json{{"role", "user"}, {"content", "(image omitted: model does not support images)"}});
        }
    }

    void appendAssistant(const AssistantMessage& message, Json& out) const {
        Json content = Json::array();
        Json calls = Json::array();
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<TextContent>(&block)) {
                if (!isBlank(text->text)) {
                    content.push_back(textChunk(text->text));
                }
            } else if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
                if (!isBlank(thinking->thinking)) {
                    content.push_back(Json{{"type", "thinking"}, {"thinking", Json::array({textChunk(thinking->thinking)})}});
                }
            } else {
                const auto& call = std::get<ToolCall>(block);
                const Json arguments = call.arguments.is_null() ? Json::object() : call.arguments;
                calls.push_back(Json{{"id", call.id},
                                     {"type", "function"},
                                     {"function", Json{{"name", call.name},
                                                       {"arguments", arguments.dump(-1, ' ', false, Json::error_handler_t::replace)}}},
                                     {"index", 0}});
            }
        }
        if (content.empty() && calls.empty()) {
            return;
        }
        Json item = Json{{"role", "assistant"}, {"prefix", false}};
        if (!content.empty()) {
            item["content"] = std::move(content);
        }
        if (!calls.empty()) {
            item["tool_calls"] = std::move(calls);
        }
        out.push_back(std::move(item));
    }

    void appendToolResult(const ToolResultMessage& message, bool supportsImages, Json& out) const {
        std::string text;
        bool first = true;
        bool hasImages = false;
        for (const auto& block : message.content) {
            if (const auto* part = std::get_if<TextContent>(&block)) {
                text += (first ? "" : "\n") + part->text;
                first = false;
            } else {
                hasImages = true;
            }
        }
        Json content = Json::array({textChunk(toolResultText(text, hasImages, supportsImages, message.isError))});
        if (supportsImages) {
            for (const auto& block : message.content) {
                if (const auto* image = std::get_if<ImageContent>(&block)) {
                    content.push_back(imageChunk(*image));
                }
            }
        }
        out.push_back(Json{{"role", "tool"},
                           {"tool_call_id", message.toolCallId},
                           {"name", message.toolName},
                           {"content", std::move(content)}});
    }

    std::string toolResultText(const std::string& text, bool hasImages, bool supportsImages, bool isError) const {
        const auto first = text.find_first_not_of(" \t\r\n");
        const std::string trimmed = first == std::string::npos
                                        ? ""
                                        : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        const std::string prefix = isError ? "[tool error] " : "";
        if (!trimmed.empty()) {
            const std::string suffix =
                hasImages && !supportsImages ? "\n[tool image omitted: model does not support images]" : "";
            return prefix + trimmed + suffix;
        }
        if (hasImages) {
            return prefix + (supportsImages ? "(see attached image)" : "(image omitted: model does not support images)");
        }
        return prefix + "(no tool output)";
    }

    Json convertMessages(const Model& model, const std::vector<Message>& messages) const {
        const bool supportsImages = std::find(model.input.begin(), model.input.end(), "image") != model.input.end();
        Json out = Json::array();
        for (std::size_t i = 0; i < messages.size(); ++i) {
            if (const auto* system = std::get_if<SystemMessage>(&messages[i])) {
                Json item = systemMessage(model, *system, i == 0);
                if (!item.is_null()) {
                    out.push_back(std::move(item));
                }
            } else if (const auto* user = std::get_if<UserMessage>(&messages[i])) {
                appendUser(*user, supportsImages, out);
            } else if (const auto* assistant = std::get_if<AssistantMessage>(&messages[i])) {
                appendAssistant(*assistant, out);
            } else {
                appendToolResult(std::get<ToolResultMessage>(messages[i]), supportsImages, out);
            }
        }
        return out;
    }

    Json convertTools(const std::vector<Tool>& tools) const {
        Json out = Json::array();
        for (const auto& tool : tools) {
            out.push_back(Json{{"type", "function"},
                               {"function", Json{{"name", tool.name},
                                                 {"description", tool.description},
                                                 {"parameters", tool.parameters},
                                                 {"strict", false}}}});
        }
        return out;
    }

    void applyReasoning(Json& payload, const Model& model, const StreamOptions& options) const {
        if (!model.reasoning) {
            return;
        }
        std::optional<ThinkingLevel> level;
        if (options.reasoning != ThinkingLevel::Off) {
            const ThinkingLevel clamped = m_levels.clamp(model, options.reasoning);
            if (clamped != ThinkingLevel::Off) {
                level = clamped;
            }
        }
        const Json& map = model.thinkingLevelMap;
        if (!map.is_object()) {
            // Models without a level map take prompt_mode instead of reasoning_effort.
            if (level) {
                payload["prompt_mode"] = "reasoning";
            }
            return;
        }
        const std::string key = level ? m_levels.levelName(*level) : "off";
        if (map.contains(key) && map[key].is_string()) {
            payload["reasoning_effort"] = map[key];
        } else if (level) {
            payload["reasoning_effort"] = "high";
        }
    }

    TranscriptNormalizer m_normalizer;
    MessageTransformer m_transformer;
    ThinkingBudgetCalculator m_budgets;
    ThinkingLevelResolver m_levels;
    ShortHash m_hash;
};
