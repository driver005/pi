module;

#include <cstdint>

export module pi.support.chat_completions_request_builder;

import std;
export import pi.support.chat_completions_compat_resolver;
export import pi.support.message_transformer;
export import pi.support.short_hash;
export import pi.support.thinking_budget_calculator;
export import pi.support.thinking_level_resolver;
export import pi.support.transcript_normalizer;
export import pi.types.chat_completions_compat;
export import pi.types.json;
export import pi.types.model;
export import pi.types.stream_options;
export import pi.types.transcript_context;

/**
 * Builds OpenAI-compatible chat completions request bodies. Port of buildParams, convertMessages
 * and convertTools in packages/ai/src/api/openai-completions.ts.
 *
 * Not ported: grammar-constrained custom tools, strict JSON-schema tools, native mid-conversation
 * tool additions (Kimi) and the OpenAI-shaped client headers (those live in the provider).
 */
export class ChatCompletionsRequestBuilder {
public:
    Json build(const Model& model, const TranscriptContext& context, const StreamOptions& options,
               std::int64_t nowMs) const;

    /** Conversation messages only. */
    Json convertMessages(const Model& model, const TranscriptContext& context,
                         const ChatCompletionsCompat& compat, std::int64_t nowMs) const;

    Json convertTools(const std::vector<Tool>& tools, const ChatCompletionsCompat& compat) const;

    /** Id accepted by chat completions: pipe-joined Responses ids are folded and shortened. */
    std::string normalizeToolCallId(const Model& model, const std::string& id) const;

    std::string retention(const StreamOptions& options) const;

private:
    bool isBlank(const std::string& text) const;
    bool hasToolHistory(const std::vector<Message>& messages) const;
    std::string imageUrl(const ImageContent& image) const;

    Json userMessage(const UserMessage& message) const;
    std::optional<Json> assistantMessage(const Model& model, const AssistantMessage& message,
                                         const ChatCompletionsCompat& compat) const;
    void toolResultMessages(const Model& model, const std::vector<Message>& messages,
                            std::size_t& index, const ChatCompletionsCompat& compat,
                            std::vector<Json>& out, bool& lastWasToolResult) const;
    Json reasoningDetailsFor(const AssistantMessage& message) const;
    std::string assistantText(const AssistantMessage& message) const;

    void applyCacheControl(Json& params, const Json& cache) const;
    bool addCacheToContent(Json& message, const Json& cache) const;
    Json compatCacheControl(const ChatCompletionsCompat& compat, const std::string& retention) const;

    void applyTokens(Json& params, const ChatCompletionsCompat& compat,
                     std::int64_t maxTokens) const;
    void applyThinking(Json& params, const Model& model, const ChatCompletionsCompat& compat,
                       std::optional<ThinkingLevel> effort,
                       std::optional<std::int64_t> budget) const;
    void applyOpenAiEffort(Json& params, const Model& model, std::optional<ThinkingLevel> effort,
                           const ChatCompletionsCompat& compat) const;
    void applyRouting(Json& params, const Model& model) const;
    Json chatTemplateValues(const Model& model, std::optional<ThinkingLevel> effort,
                            const Json& values, std::optional<std::int64_t> budget) const;
    std::optional<Json> resolveTemplateValue(const Model& model, std::optional<ThinkingLevel> effort,
                                             const Json& value,
                                             std::optional<std::int64_t> budget) const;

    /** thinkingLevelMap[name] with "absent" and "null" kept apart. */
    std::optional<Json> mapEntry(const Model& model, const std::string& name) const;
    /** map[name] when it is a string, else name itself. */
    std::string mappedOrSelf(const Model& model, const std::string& name) const;
    bool offHidden(const Model& model) const;

    TranscriptNormalizer m_normalizer;
    MessageTransformer m_transformer;
    ThinkingBudgetCalculator m_budgets;
    ThinkingLevelResolver m_levels;
    ChatCompletionsCompatResolver m_compat;
    ShortHash m_hash;
};

bool ChatCompletionsRequestBuilder::isBlank(const std::string& text) const {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::string ChatCompletionsRequestBuilder::imageUrl(const ImageContent& image) const {
    return "data:" + image.mimeType + ";base64," + image.data;
}

bool ChatCompletionsRequestBuilder::hasToolHistory(const std::vector<Message>& messages) const {
    for (const auto& message : messages) {
        if (std::holds_alternative<ToolResultMessage>(message)) {
            return true;
        }
        if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            for (const auto& block : assistant->content) {
                if (std::holds_alternative<ToolCall>(block)) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::string ChatCompletionsRequestBuilder::retention(const StreamOptions& options) const {
    if (options.cacheRetention) {
        return *options.cacheRetention;
    }
    const auto env = options.env.find("PI_CACHE_RETENTION");
    return env != options.env.end() && env->second == "long" ? "long" : "short";
}

std::string ChatCompletionsRequestBuilder::normalizeToolCallId(const Model& model,
                                                               const std::string& id) const {
    const auto fold = [this](const std::string& text) {
        return m_transformer.sanitizeToolCallId(text, text.size());
    };
    const auto pipe = id.find('|');
    if (pipe != std::string::npos) {
        const std::string callId = fold(id.substr(0, pipe));
        const std::string itemId = fold(id.substr(pipe + 1));
        const std::string combined = itemId.empty() ? callId : callId + "_" + itemId;
        if (combined.size() <= 40) {
            return combined;
        }
        const std::string hash = m_hash.of(id).substr(0, 8);
        const std::size_t room = std::max<std::size_t>(1, 40 - hash.size() - 1);
        return callId.substr(0, room) + "_" + hash;
    }
    if (model.provider == "openai" && id.size() > 40) {
        return id.substr(0, 40);
    }
    return id;
}

Json ChatCompletionsRequestBuilder::userMessage(const UserMessage& message) const {
    Json out = Json::object();
    out["role"] = "user";
    if (const auto* text = std::get_if<std::string>(&message.content)) {
        out["content"] = *text;
        return out;
    }
    Json parts = Json::array();
    for (const auto& block : std::get<std::vector<UserContentBlock>>(message.content)) {
        if (const auto* text = std::get_if<TextContent>(&block)) {
            if (text->text.empty()) {
                continue;
            }
            Json part = Json::object();
            part["type"] = "text";
            part["text"] = text->text;
            parts.push_back(std::move(part));
        } else {
            Json url = Json::object();
            url["url"] = imageUrl(std::get<ImageContent>(block));
            Json part = Json::object();
            part["type"] = "image_url";
            part["image_url"] = std::move(url);
            parts.push_back(std::move(part));
        }
    }
    out["content"] = std::move(parts);
    return out;
}

std::string ChatCompletionsRequestBuilder::assistantText(const AssistantMessage& message) const {
    std::string text;
    for (const auto& block : message.content) {
        if (const auto* part = std::get_if<TextContent>(&block)) {
            if (!isBlank(part->text)) {
                text += part->text;
            }
        }
    }
    return text;
}

Json ChatCompletionsRequestBuilder::reasoningDetailsFor(const AssistantMessage& message) const {
    for (const auto& block : message.content) {
        const auto* thinking = std::get_if<ThinkingContent>(&block);
        if (thinking == nullptr || !thinking->thinkingSignature) {
            continue;
        }
        const Json parsed = Json::parse(*thinking->thinkingSignature, nullptr, false);
        if (parsed.is_array() && !parsed.empty() &&
            std::all_of(parsed.begin(), parsed.end(), [](const Json& item) {
                return item.is_object() && item.contains("type") && item["type"].is_string();
            })) {
            return parsed;
        }
    }
    return Json();
}

std::optional<Json> ChatCompletionsRequestBuilder::assistantMessage(
    const Model& model, const AssistantMessage& message, const ChatCompletionsCompat& compat) const {
    Json out = Json::object();
    out["role"] = "assistant";
    out["content"] = compat.requiresAssistantAfterToolResult ? Json("") : Json(nullptr);
    const std::string text = assistantText(message);
    std::vector<const ThinkingContent*> thinking;
    Json toolCalls = Json::array();
    for (const auto& block : message.content) {
        if (const auto* t = std::get_if<ThinkingContent>(&block)) {
            if (!isBlank(t->thinking)) {
                thinking.push_back(t);
            }
        } else if (const auto* call = std::get_if<ToolCall>(&block)) {
            Json function = Json::object();
            function["name"] = call->name;
            function["arguments"] = call->arguments.dump(-1, ' ', false, Json::error_handler_t::replace);
            Json entry = Json::object();
            entry["id"] = call->id;
            entry["type"] = "function";
            entry["function"] = std::move(function);
            toolCalls.push_back(std::move(entry));
        }
    }
    const Json details = reasoningDetailsFor(message);
    if (!thinking.empty() && compat.requiresThinkingAsText) {
        std::string joined;
        for (std::size_t i = 0; i < thinking.size(); ++i) {
            joined += (i > 0 ? "\n\n" : "") + thinking[i]->thinking;
        }
        Json parts = Json::array();
        Json first = Json::object();
        first["type"] = "text";
        first["text"] = joined;
        parts.push_back(std::move(first));
        if (!text.empty()) {
            Json second = Json::object();
            second["type"] = "text";
            second["text"] = text;
            parts.push_back(std::move(second));
        }
        out["content"] = std::move(parts);
    } else {
        if (!text.empty()) {
            out["content"] = text;
        }
        if (!thinking.empty() && details.is_null()) {
            const std::string signature = thinking.front()->thinkingSignature.value_or("");
            const std::string field = model.provider == "opencode-go" && signature == "reasoning"
                                          ? "reasoning_content"
                                          : signature;
            if (field == "reasoning" || field == "reasoning_content" || field == "reasoning_text") {
                std::string joined;
                for (std::size_t i = 0; i < thinking.size(); ++i) {
                    joined += (i > 0 ? "\n" : "") + thinking[i]->thinking;
                }
                out[field] = joined;
            }
        }
    }
    if (!toolCalls.empty()) {
        out["tool_calls"] = std::move(toolCalls);
    }
    if (!details.is_null()) {
        out["reasoning_details"] = details;
    }
    if (compat.requiresReasoningContentOnAssistantMessages && model.reasoning &&
        !out.contains("reasoning_content")) {
        out["reasoning_content"] = "";
    }
    const Json& content = out["content"];
    const bool hasContent = !content.is_null() && !content.empty();
    if (!hasContent && !out.contains("tool_calls")) {
        return std::nullopt;
    }
    return out;
}

void ChatCompletionsRequestBuilder::toolResultMessages(
    const Model& model, const std::vector<Message>& messages, std::size_t& index,
    const ChatCompletionsCompat& compat, std::vector<Json>& out, bool& lastWasToolResult) const {
    Json images = Json::array();
    std::size_t j = index;
    for (; j < messages.size() && std::holds_alternative<ToolResultMessage>(messages[j]); ++j) {
        const auto& result = std::get<ToolResultMessage>(messages[j]);
        std::string text;
        bool first = true;
        bool hasImages = false;
        for (const auto& block : result.content) {
            if (const auto* part = std::get_if<TextContent>(&block)) {
                text += (first ? "" : "\n") + part->text;
                first = false;
            } else {
                hasImages = true;
            }
        }
        Json entry = Json::object();
        entry["role"] = "tool";
        entry["content"] = !text.empty() ? text : hasImages ? "(see attached image)" : "(no tool output)";
        entry["tool_call_id"] = result.toolCallId;
        if (compat.requiresToolResultName && !result.toolName.empty()) {
            entry["name"] = result.toolName;
        }
        out.push_back(std::move(entry));
        const bool modelSeesImages =
            std::find(model.input.begin(), model.input.end(), "image") != model.input.end();
        if (hasImages && modelSeesImages) {
            for (const auto& block : result.content) {
                if (const auto* image = std::get_if<ImageContent>(&block)) {
                    Json url = Json::object();
                    url["url"] = imageUrl(*image);
                    Json part = Json::object();
                    part["type"] = "image_url";
                    part["image_url"] = std::move(url);
                    images.push_back(std::move(part));
                }
            }
        }
    }
    index = j - 1;
    if (images.empty()) {
        lastWasToolResult = true;
        return;
    }
    if (compat.requiresAssistantAfterToolResult) {
        Json bridge = Json::object();
        bridge["role"] = "assistant";
        bridge["content"] = "I have processed the tool results.";
        out.push_back(std::move(bridge));
    }
    Json parts = Json::array();
    Json intro = Json::object();
    intro["type"] = "text";
    intro["text"] = "Attached image(s) from tool result:";
    parts.push_back(std::move(intro));
    for (auto& image : images) {
        parts.push_back(std::move(image));
    }
    Json user = Json::object();
    user["role"] = "user";
    user["content"] = std::move(parts);
    out.push_back(std::move(user));
    lastWasToolResult = false;
}

Json ChatCompletionsRequestBuilder::convertMessages(const Model& model,
                                                    const TranscriptContext& rawContext,
                                                    const ChatCompletionsCompat& compat,
                                                    std::int64_t nowMs) const {
    const TranscriptContext context = m_normalizer.resolveTranscript(rawContext, false);
    const auto normalizeId = [this](const std::string& id, const Model& target, const AssistantMessage&) {
        return normalizeToolCallId(target, id);
    };
    const std::vector<Message> messages =
        m_transformer.transform(context.messages, model, normalizeId, nowMs);
    const std::string instructionRole =
        model.reasoning && compat.supportsDeveloperRole ? "developer" : "system";
    std::vector<Json> out;
    bool lastWasToolResult = false;
    std::string lastRole;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const Message& message = messages[i];
        const bool isUser = std::holds_alternative<UserMessage>(message);
        if (compat.requiresAssistantAfterToolResult && lastRole == "toolResult" && isUser) {
            Json bridge = Json::object();
            bridge["role"] = "assistant";
            bridge["content"] = "I have processed the tool results.";
            out.push_back(std::move(bridge));
        }
        if (const auto* system = std::get_if<SystemMessage>(&message)) {
            const std::string text = i == 0 ? m_normalizer.systemMessageText(*system)
                                            : m_normalizer.renderSystemMessageUpdate(*system);
            if (!text.empty()) {
                Json entry = Json::object();
                entry["role"] = instructionRole;
                entry["content"] = text;
                out.push_back(std::move(entry));
            }
            lastRole = "system";
        } else if (const auto* user = std::get_if<UserMessage>(&message)) {
            Json converted = userMessage(*user);
            const bool empty = converted["content"].is_array() && converted["content"].empty();
            if (!empty) {
                out.push_back(std::move(converted));
            }
            lastRole = "user";
        } else if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            if (auto converted = assistantMessage(model, *assistant, compat)) {
                out.push_back(std::move(*converted));
            }
            lastRole = "assistant";
        } else {
            toolResultMessages(model, messages, i, compat, out, lastWasToolResult);
            lastRole = lastWasToolResult ? "toolResult" : "user";
        }
    }
    return Json(out);
}

Json ChatCompletionsRequestBuilder::convertTools(const std::vector<Tool>& tools,
                                                 const ChatCompletionsCompat& compat) const {
    Json out = Json::array();
    for (const auto& tool : tools) {
        Json function = Json::object();
        function["name"] = tool.name;
        function["description"] = tool.description;
        function["parameters"] = tool.parameters;
        if (compat.supportsStrictMode) {
            function["strict"] = false;
        }
        Json entry = Json::object();
        entry["type"] = "function";
        entry["function"] = std::move(function);
        out.push_back(std::move(entry));
    }
    return out;
}

Json ChatCompletionsRequestBuilder::compatCacheControl(const ChatCompletionsCompat& compat,
                                                       const std::string& retention) const {
    if (compat.cacheControlFormat != "anthropic" || retention == "none") {
        return Json();
    }
    Json control = Json::object();
    control["type"] = "ephemeral";
    if (retention == "long" && compat.supportsLongCacheRetention) {
        control["ttl"] = "1h";
    }
    return control;
}

bool ChatCompletionsRequestBuilder::addCacheToContent(Json& message, const Json& cache) const {
    Json& content = message["content"];
    if (content.is_string()) {
        if (content.get<std::string>().empty()) {
            return false;
        }
        Json part = Json::object();
        part["type"] = "text";
        part["text"] = content;
        part["cache_control"] = cache;
        content = Json::array({std::move(part)});
        return true;
    }
    if (!content.is_array()) {
        return false;
    }
    for (std::size_t i = content.size(); i-- > 0;) {
        if (content[i].value("type", "") == "text") {
            content[i]["cache_control"] = cache;
            return true;
        }
    }
    return false;
}

void ChatCompletionsRequestBuilder::applyCacheControl(Json& params, const Json& cache) const {
    Json& messages = params["messages"];
    for (auto& message : messages) {
        const std::string role = message.value("role", "");
        if (role == "system" || role == "developer") {
            addCacheToContent(message, cache);
            break;
        }
    }
    if (params.contains("tools") && !params["tools"].empty()) {
        params["tools"].back()["cache_control"] = cache;
    }
    for (std::size_t i = messages.size(); i-- > 0;) {
        const std::string role = messages[i].value("role", "");
        if ((role == "user" || role == "assistant" || role == "tool") &&
            addCacheToContent(messages[i], cache)) {
            break;
        }
    }
}

void ChatCompletionsRequestBuilder::applyTokens(Json& params, const ChatCompletionsCompat& compat,
                                                std::int64_t maxTokens) const {
    if (maxTokens > 0) {
        params[compat.maxTokensField == "max_tokens" ? "max_tokens" : "max_completion_tokens"] = maxTokens;
    }
}

std::optional<Json> ChatCompletionsRequestBuilder::mapEntry(const Model& model,
                                                            const std::string& name) const {
    if (model.thinkingLevelMap.is_object() && model.thinkingLevelMap.contains(name)) {
        return model.thinkingLevelMap[name];
    }
    return std::nullopt;
}

std::string ChatCompletionsRequestBuilder::mappedOrSelf(const Model& model,
                                                        const std::string& name) const {
    const auto entry = mapEntry(model, name);
    if (entry && entry->is_string()) {
        return entry->get<std::string>();
    }
    return name;
}

bool ChatCompletionsRequestBuilder::offHidden(const Model& model) const {
    const auto entry = mapEntry(model, "off");
    return entry && entry->is_null();
}

std::optional<Json> ChatCompletionsRequestBuilder::resolveTemplateValue(
    const Model& model, std::optional<ThinkingLevel> effort, const Json& value,
    std::optional<std::int64_t> budget) const {
    if (!value.is_object()) {
        return value;
    }
    if (!effort && value.value("omitWhenOff", false)) {
        return std::nullopt;
    }
    const std::string variable = value.value("$var", "");
    if (variable == "thinking.enabled") {
        return Json(effort.has_value());
    }
    if (variable == "thinking.budget") {
        if (!budget) {
            return std::nullopt;
        }
        return Json(*budget);
    }
    const std::string name = effort ? m_levels.levelName(*effort) : "off";
    const auto entry = mapEntry(model, name);
    if (!entry) {
        return effort ? std::optional<Json>(Json(name)) : std::nullopt;
    }
    if (entry->is_string()) {
        return *entry;
    }
    return std::nullopt;
}

Json ChatCompletionsRequestBuilder::chatTemplateValues(const Model& model,
                                                       std::optional<ThinkingLevel> effort,
                                                       const Json& values,
                                                       std::optional<std::int64_t> budget) const {
    Json out = Json::object();
    if (!values.is_object()) {
        return out;
    }
    for (const auto& entry : values.items()) {
        const std::string& key = entry.key();
        const Json& value = entry.value();
        if (auto resolved = resolveTemplateValue(model, effort, value, budget)) {
            out[key] = std::move(*resolved);
        }
    }
    return out;
}

void ChatCompletionsRequestBuilder::applyOpenAiEffort(Json& params, const Model& model,
                                                      std::optional<ThinkingLevel> effort,
                                                      const ChatCompletionsCompat& compat) const {
    if (effort && model.reasoning && compat.supportsReasoningEffort) {
        params["reasoning_effort"] = mappedOrSelf(model, m_levels.levelName(*effort));
    } else if (!effort && model.reasoning && compat.supportsReasoningEffort) {
        const auto off = mapEntry(model, "off");
        if (off && off->is_string()) {
            params["reasoning_effort"] = *off;
        }
    }
}

void ChatCompletionsRequestBuilder::applyThinking(Json& params, const Model& model,
                                                  const ChatCompletionsCompat& compat,
                                                  std::optional<ThinkingLevel> effort,
                                                  std::optional<std::int64_t> budget) const {
    if (!model.reasoning) {
        return;
    }
    const std::string format = compat.thinkingFormat;
    const std::string effortName = effort ? m_levels.levelName(*effort) : "";
    if (format == "zai") {
        Json thinking = Json::object();
        thinking["type"] = effort ? "enabled" : "disabled";
        if (effort) {
            thinking["clear_thinking"] = false;
        }
        params["thinking"] = std::move(thinking);
        if (effort && compat.supportsReasoningEffort) {
            const auto entry = mapEntry(model, effortName);
            if (!entry) {
                params["reasoning_effort"] = effortName;
            } else if (entry->is_string()) {
                params["reasoning_effort"] = *entry;
            }
        }
    } else if (format == "qwen") {
        params["enable_thinking"] = effort.has_value();
        if (effort && compat.supportsReasoningEffort) {
            params["reasoning_effort"] = mappedOrSelf(model, effortName);
        }
    } else if (format == "qwen-chat-template") {
        Json kwargs = Json::object();
        kwargs["enable_thinking"] = effort.has_value();
        kwargs["preserve_thinking"] = true;
        params["chat_template_kwargs"] = std::move(kwargs);
    } else if (format == "chat-template") {
        Json kwargs = chatTemplateValues(model, effort, compat.chatTemplateKwargs, budget);
        if (!kwargs.empty()) {
            params["chat_template_kwargs"] = std::move(kwargs);
        }
    } else if (format == "baseten") {
        Json args = chatTemplateValues(model, effort, compat.chatTemplateArgs, budget);
        if (!args.empty()) {
            params["chat_template_args"] = std::move(args);
        }
        if (compat.supportsReasoningEffort) {
            const auto entry = mapEntry(model, effort ? effortName : "off");
            if (!entry && effort) {
                params["reasoning_effort"] = effortName;
            } else if (entry && entry->is_string()) {
                params["reasoning_effort"] = *entry;
            }
        }
    } else if (format == "deepseek") {
        if (effort) {
            Json thinking = Json::object();
            thinking["type"] = "enabled";
            params["thinking"] = std::move(thinking);
        } else if (!offHidden(model)) {
            Json thinking = Json::object();
            thinking["type"] = "disabled";
            params["thinking"] = std::move(thinking);
        }
        if (effort && compat.supportsReasoningEffort) {
            params["reasoning_effort"] = mappedOrSelf(model, effortName);
        }
    } else if (format == "openrouter") {
        Json reasoning = Json::object();
        if (effort) {
            reasoning["effort"] = mappedOrSelf(model, effortName);
            params["reasoning"] = std::move(reasoning);
        } else if (!offHidden(model)) {
            const auto off = mapEntry(model, "off");
            reasoning["effort"] = off && off->is_string() ? *off : Json("none");
            params["reasoning"] = std::move(reasoning);
        }
    } else if (format == "ant-ling") {
        if (effort) {
            const auto entry = mapEntry(model, effortName);
            if (entry && entry->is_string()) {
                Json reasoning = Json::object();
                reasoning["effort"] = *entry;
                params["reasoning"] = std::move(reasoning);
            }
        }
    } else if (format == "together") {
        Json reasoning = Json::object();
        reasoning["enabled"] = effort.has_value();
        params["reasoning"] = std::move(reasoning);
        if (effort && compat.supportsReasoningEffort) {
            params["reasoning_effort"] = mappedOrSelf(model, effortName);
        }
    } else if (format == "string-thinking") {
        if (effort) {
            params["thinking"] = mappedOrSelf(model, effortName);
        } else if (!offHidden(model)) {
            const auto off = mapEntry(model, "off");
            params["thinking"] = off && off->is_string() ? *off : Json("none");
        }
    } else {
        applyOpenAiEffort(params, model, effort, compat);
    }
}

void ChatCompletionsRequestBuilder::applyRouting(Json& params, const Model& model) const {
    if (!model.compat.is_object()) {
        return;
    }
    if (model.compat.contains("openRouterRouting") && model.compat["openRouterRouting"].is_object() &&
        !model.compat["openRouterRouting"].empty()) {
        params["provider"] = model.compat["openRouterRouting"];
    }
    if (model.compat.contains("vercelGatewayRouting") && model.compat["vercelGatewayRouting"].is_object()) {
        const Json& routing = model.compat["vercelGatewayRouting"];
        Json gateway = Json::object();
        if (routing.contains("only")) {
            gateway["only"] = routing["only"];
        }
        if (routing.contains("order")) {
            gateway["order"] = routing["order"];
        }
        if (!gateway.empty()) {
            Json options = Json::object();
            options["gateway"] = std::move(gateway);
            params["providerOptions"] = std::move(options);
        }
    }
}

Json ChatCompletionsRequestBuilder::build(const Model& model, const TranscriptContext& rawContext,
                                          const StreamOptions& options, std::int64_t nowMs) const {
    const ChatCompletionsCompat compat = m_compat.resolve(model);
    const TranscriptContext context = m_normalizer.resolveTranscript(rawContext, false);
    const std::string cacheMode = retention(options);
    const std::int64_t maxTokens = m_budgets.clampMaxTokensToContext(
        model, context, options.maxTokens.value_or(model.maxTokens));
    std::optional<ThinkingLevel> effort;
    if (options.reasoning != ThinkingLevel::Off) {
        const ThinkingLevel clamped = m_levels.clamp(model, options.reasoning);
        if (clamped != ThinkingLevel::Off) {
            effort = clamped;
        }
    }

    Json params = Json::object();
    params["model"] = model.id;
    params["messages"] = convertMessages(model, context, compat, nowMs);
    params["stream"] = true;
    const bool longCache = cacheMode == "long" && compat.supportsLongCacheRetention;
    const bool keyed = (model.baseUrl.find("api.openai.com") != std::string::npos && cacheMode != "none") ||
                       longCache;
    if (keyed && options.sessionId) {
        std::string key = *options.sessionId;
        // The cache key is capped at 64 characters, counted as code points.
        std::size_t points = 0;
        std::size_t cut = key.size();
        for (std::size_t i = 0; i < key.size(); ++i) {
            if ((static_cast<unsigned char>(key[i]) & 0xC0) != 0x80 && ++points > 64) {
                cut = i;
                break;
            }
        }
        params["prompt_cache_key"] = key.substr(0, cut);
    }
    if (longCache) {
        params["prompt_cache_retention"] = "24h";
    }
    if (compat.supportsUsageInStreaming) {
        Json streamOptions = Json::object();
        streamOptions["include_usage"] = true;
        params["stream_options"] = std::move(streamOptions);
    }
    if (compat.supportsStore) {
        params["store"] = false;
    }
    applyTokens(params, compat, maxTokens);
    if (options.temperature) {
        params["temperature"] = *options.temperature;
    }
    const std::vector<Tool> tools = m_normalizer.currentTools(context.messages);
    if (!tools.empty()) {
        params["tools"] = convertTools(tools, compat);
        if (compat.zaiToolStream) {
            params["tool_stream"] = true;
        }
    } else if (hasToolHistory(context.messages)) {
        params["tools"] = Json::array();
    }
    const Json cache = compatCacheControl(compat, cacheMode);
    if (!cache.is_null()) {
        applyCacheControl(params, cache);
    }
    if (options.toolChoice) {
        params["tool_choice"] = *options.toolChoice;
    }
    if (!compat.vllmPriority.is_null()) {
        params["priority"] = compat.vllmPriority;
    }
    std::optional<std::int64_t> budget;
    if (effort && model.reasoning) {
        const std::int64_t ceiling = maxTokens > 0 ? maxTokens : model.maxTokens;
        const std::int64_t raw = m_budgets.budgetForLevel(*effort, options.thinkingBudgets);
        const std::int64_t clamped = std::min(raw, std::max<std::int64_t>(0, ceiling - 1024));
        if (clamped > 0) {
            budget = clamped;
        }
    }
    applyThinking(params, model, compat, effort, budget);
    std::optional<std::string> budgetField = compat.thinkingTokenBudgetField;
    if (!budgetField && compat.supportsThinkingTokenBudget) {
        budgetField = "thinking_token_budget";
    }
    if (budgetField && budget) {
        params[*budgetField] = *budget;
    }
    applyRouting(params, model);
    for (const Json* extra : {&model.samplingParams, &options.samplingParams}) {
        if (extra->is_object()) {
            for (const auto& entry : extra->items()) {
                const std::string& key = entry.key();
                const Json& value = entry.value();
                params[key] = value;
            }
        }
    }
    return params;
}
