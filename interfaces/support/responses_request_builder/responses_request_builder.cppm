module;

#include <cstdint>

export module pi.support.responses_request_builder;

import std;
export import pi.support.message_transformer;
export import pi.support.responses_compat_resolver;
export import pi.support.short_hash;
export import pi.support.thinking_budget_calculator;
export import pi.support.thinking_level_resolver;
export import pi.support.transcript_normalizer;
export import pi.types.json;
export import pi.types.model;
export import pi.types.responses_compat;
export import pi.types.stream_options;
export import pi.types.transcript_context;

/**
 * Builds OpenAI Responses request bodies. Port of buildParams in api/openai-responses.ts and of
 * convertResponsesMessages / convertResponsesTools in api/openai-responses-shared.ts.
 *
 * Not ported: grammar-constrained custom tools, strict JSON-schema sampling, additional_tools and
 * tool_search items, service tiers.
 */
export class ResponsesRequestBuilder {
public:
    /**
     * The full request body of a streaming call (api.openai.com style). toolCallProviders are the
     * providers whose "callId|itemId" tool call ids are kept in pair form.
     */
    Json build(const Model& model, const TranscriptContext& context, const StreamOptions& options,
               std::int64_t nowMs,
               const std::set<std::string>& toolCallProviders = {"openai", "openai-codex", "opencode"}) const;

    /**
     * The `input` items. allowedToolCallProviders are the providers whose "callId|itemId" tool
     * call ids are kept in pair form; includeSystemPrompt false leaves the leading system message
     * out (for endpoints that take `instructions`).
     */
    Json convertMessages(const Model& model, const TranscriptContext& context, const ResponsesCompat& compat,
                         const std::set<std::string>& allowedToolCallProviders, bool includeSystemPrompt,
                         std::int64_t nowMs) const;

    Json convertTools(const std::vector<Tool>& tools, const ResponsesCompat& compat) const;

    std::string retention(const StreamOptions& options) const;

private:
    Json systemItem(const std::string& role, const std::string& text) const;
    void appendUser(const UserMessage& message, Json& out) const;
    void appendAssistant(const Model& model, const AssistantMessage& message, std::size_t msgIndex,
                         Json& out) const;
    void appendAssistantText(const TextContent& block, std::size_t msgIndex, std::size_t textIndex,
                             Json& items) const;
    Json functionCall(const ToolCall& call, bool sameModel, bool differentModel) const;
    void appendToolResult(const Model& model, const ToolResultMessage& message, Json& out) const;
    Json toolResultOutput(const Model& model, const std::vector<UserContentBlock>& content) const;
    std::string normalizeIdPart(const std::string& part) const;
    std::string normalizeToolCallId(const Model& model, const std::set<std::string>& allowed,
                                    const std::string& id, const AssistantMessage& source) const;
    std::optional<std::pair<std::string, std::optional<std::string>>> parseTextSignature(
        const std::optional<std::string>& signature) const;
    std::string clampCacheKey(const std::string& key) const;
    bool isChatGptSignIn(const Model& model, const StreamOptions& options) const;
    void applyCache(Json& params, const ResponsesCompat& compat, const StreamOptions& options,
                    bool omitUnsupported) const;
    void applyReasoning(Json& params, const Model& model, const StreamOptions& options) const;
    std::optional<Json> mapEntry(const Model& model, const std::string& name) const;
    bool supportsImages(const Model& model) const;

    TranscriptNormalizer m_normalizer;
    MessageTransformer m_transformer;
    ThinkingBudgetCalculator m_budgets;
    ThinkingLevelResolver m_levels;
    ResponsesCompatResolver m_compat;
    ShortHash m_hash;
};

std::string ResponsesRequestBuilder::retention(const StreamOptions& options) const {
    if (options.cacheRetention) {
        return *options.cacheRetention;
    }
    const auto env = options.env.find("PI_CACHE_RETENTION");
    return env != options.env.end() && env->second == "long" ? "long" : "short";
}

bool ResponsesRequestBuilder::supportsImages(const Model& model) const {
    return std::find(model.input.begin(), model.input.end(), "image") != model.input.end();
}

std::optional<Json> ResponsesRequestBuilder::mapEntry(const Model& model, const std::string& name) const {
    if (model.thinkingLevelMap.is_object() && model.thinkingLevelMap.contains(name)) {
        return model.thinkingLevelMap[name];
    }
    return std::nullopt;
}

std::string ResponsesRequestBuilder::normalizeIdPart(const std::string& part) const {
    std::string out;
    for (const char c : part) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
        out.push_back(ok ? c : '_');
    }
    if (out.size() > 64) {
        out.resize(64);
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out;
}

std::string ResponsesRequestBuilder::normalizeToolCallId(const Model& model,
                                                         const std::set<std::string>& allowed,
                                                         const std::string& id,
                                                         const AssistantMessage& source) const {
    const auto pipe = id.find('|');
    if (!allowed.contains(model.provider) || pipe == std::string::npos) {
        return normalizeIdPart(id);
    }
    const std::string callId = normalizeIdPart(id.substr(0, pipe));
    const std::string rawItem = id.substr(pipe + 1);
    const bool foreign = source.provider != model.provider || source.api != model.api;
    std::string itemId;
    if (foreign) {
        itemId = "fc_" + m_hash.of(rawItem);
        if (itemId.size() > 64) {
            itemId.resize(64);
        }
    } else {
        itemId = normalizeIdPart(rawItem);
    }
    if (!itemId.starts_with("fc_")) {
        itemId = normalizeIdPart("fc_" + itemId);
    }
    return callId + "|" + itemId;
}

std::optional<std::pair<std::string, std::optional<std::string>>> ResponsesRequestBuilder::parseTextSignature(
    const std::optional<std::string>& signature) const {
    if (!signature || signature->empty()) {
        return std::nullopt;
    }
    if (signature->starts_with("{")) {
        const Json parsed = Json::parse(*signature, nullptr, false);
        if (parsed.is_object() && parsed.value("v", 0) == 1 && parsed.contains("id") && parsed["id"].is_string()) {
            const std::string phase = parsed.value("phase", "");
            const bool knownPhase = phase == "commentary" || phase == "final_answer";
            return std::make_pair(parsed["id"].get<std::string>(),
                                  knownPhase ? std::optional<std::string>(phase) : std::nullopt);
        }
    }
    return std::make_pair(*signature, std::optional<std::string>());
}

Json ResponsesRequestBuilder::systemItem(const std::string& role, const std::string& text) const {
    Json item = Json::object();
    item["role"] = role;
    item["content"] = text;
    return item;
}

void ResponsesRequestBuilder::appendUser(const UserMessage& message, Json& out) const {
    Json content = Json::array();
    if (const auto* text = std::get_if<std::string>(&message.content)) {
        content.push_back(Json{{"type", "input_text"}, {"text", *text}});
    } else {
        for (const auto& block : std::get<std::vector<UserContentBlock>>(message.content)) {
            if (const auto* text = std::get_if<TextContent>(&block)) {
                content.push_back(Json{{"type", "input_text"}, {"text", text->text}});
            } else {
                const auto& image = std::get<ImageContent>(block);
                content.push_back(Json{{"type", "input_image"},
                                       {"detail", "auto"},
                                       {"image_url", "data:" + image.mimeType + ";base64," + image.data}});
            }
        }
    }
    if (!content.empty()) {
        out.push_back(Json{{"role", "user"}, {"content", std::move(content)}});
    }
}

void ResponsesRequestBuilder::appendAssistantText(const TextContent& block, std::size_t msgIndex,
                                                  std::size_t textIndex, Json& items) const {
    const auto parsed = parseTextSignature(block.textSignature);
    std::string id;
    if (!parsed) {
        id = textIndex == 0 ? "msg_pi_" + std::to_string(msgIndex)
                            : "msg_pi_" + std::to_string(msgIndex) + "_" + std::to_string(textIndex);
    } else if (parsed->first.size() > 64) {
        id = "msg_" + m_hash.of(parsed->first);
    } else {
        id = parsed->first;
    }
    Json item = Json::object();
    item["type"] = "message";
    item["role"] = "assistant";
    item["content"] = Json::array({Json{{"type", "output_text"}, {"text", block.text}, {"annotations", Json::array()}}});
    item["status"] = "completed";
    item["id"] = id;
    if (parsed && parsed->second) {
        item["phase"] = *parsed->second;
    }
    items.push_back(std::move(item));
}

Json ResponsesRequestBuilder::functionCall(const ToolCall& call, bool sameModel, bool differentModel) const {
    const auto pipe = call.id.find('|');
    const std::string callId = call.id.substr(0, pipe);
    std::optional<std::string> itemId;
    if (pipe != std::string::npos) {
        itemId = call.id.substr(pipe + 1);
    }
    // Ids of another model's items would fail OpenAI's reasoning pairing validation.
    if (differentModel || !itemId || !itemId->starts_with("fc_")) {
        itemId.reset();
    }
    Json item = Json::object();
    item["type"] = "function_call";
    if (itemId) {
        item["id"] = *itemId;
    }
    item["call_id"] = callId;
    item["name"] = call.name;
    item["arguments"] = call.arguments.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (sameModel && call.toolNamespace) {
        item["namespace"] = *call.toolNamespace;
    }
    return item;
}

void ResponsesRequestBuilder::appendAssistant(const Model& model, const AssistantMessage& message,
                                              std::size_t msgIndex, Json& out) const {
    const bool sameProviderAndApi = message.provider == model.provider && message.api == model.api;
    const bool sameModel = sameProviderAndApi && message.model == model.id;
    const bool differentModel = sameProviderAndApi && message.model != model.id;
    Json items = Json::array();
    std::size_t textIndex = 0;
    for (const auto& block : message.content) {
        if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
            if (thinking->thinkingSignature) {
                Json reasoning = Json::parse(*thinking->thinkingSignature, nullptr, false);
                if (reasoning.is_object()) {
                    items.push_back(std::move(reasoning));
                }
            }
        } else if (const auto* text = std::get_if<TextContent>(&block)) {
            appendAssistantText(*text, msgIndex, textIndex++, items);
        } else {
            items.push_back(functionCall(std::get<ToolCall>(block), sameModel, differentModel));
        }
    }
    for (auto& item : items) {
        out.push_back(std::move(item));
    }
}

Json ResponsesRequestBuilder::toolResultOutput(const Model& model, const std::vector<UserContentBlock>& content) const {
    std::string text;
    bool first = true;
    std::vector<const ImageContent*> images;
    for (const auto& block : content) {
        if (const auto* part = std::get_if<TextContent>(&block)) {
            text += (first ? "" : "\n") + part->text;
            first = false;
        } else {
            images.push_back(&std::get<ImageContent>(block));
        }
    }
    if (images.empty() || !supportsImages(model)) {
        return !text.empty() ? text : !images.empty() ? "(see attached image)" : "(no tool output)";
    }
    Json output = Json::array();
    if (!text.empty()) {
        output.push_back(Json{{"type", "input_text"}, {"text", text}});
    }
    for (const auto* image : images) {
        output.push_back(Json{{"type", "input_image"},
                              {"detail", "auto"},
                              {"image_url", "data:" + image->mimeType + ";base64," + image->data}});
    }
    return output;
}

void ResponsesRequestBuilder::appendToolResult(const Model& model, const ToolResultMessage& message, Json& out) const {
    Json item = Json::object();
    item["type"] = "function_call_output";
    item["call_id"] = message.toolCallId.substr(0, message.toolCallId.find('|'));
    item["output"] = toolResultOutput(model, message.content);
    out.push_back(std::move(item));
}

Json ResponsesRequestBuilder::convertMessages(const Model& model, const TranscriptContext& rawContext,
                                              const ResponsesCompat& compat,
                                              const std::set<std::string>& allowedToolCallProviders,
                                              bool includeSystemPrompt, std::int64_t nowMs) const {
    const TranscriptContext context = m_normalizer.resolveTranscript(rawContext, compat.supportsMidConvoSystemMessages);
    const auto transformed = m_transformer.transform(
        context.messages, model,
        [&](const std::string& id, const Model&, const AssistantMessage& source) {
            return normalizeToolCallId(model, allowedToolCallProviders, id, source);
        },
        nowMs);
    const std::string instructionRole = model.reasoning && compat.supportsDeveloperRole ? "developer" : "system";
    Json out = Json::array();
    std::size_t msgIndex = 0;
    for (std::size_t source = 0; source < transformed.size(); ++source) {
        const Message& message = transformed[source];
        const bool leadingSystem = source == 0 && std::holds_alternative<SystemMessage>(message);
        if (const auto* system = std::get_if<SystemMessage>(&message)) {
            if (!leadingSystem || includeSystemPrompt) {
                const std::string text = leadingSystem ? m_normalizer.systemMessageText(*system)
                                                       : m_normalizer.renderSystemMessageUpdate(*system);
                if (!text.empty()) {
                    out.push_back(systemItem(instructionRole, text));
                }
            }
        } else if (const auto* user = std::get_if<UserMessage>(&message)) {
            appendUser(*user, out);
        } else if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            appendAssistant(model, *assistant, msgIndex, out);
        } else {
            appendToolResult(model, std::get<ToolResultMessage>(message), out);
        }
        if (!leadingSystem) {
            ++msgIndex;
        }
    }
    return out;
}

Json ResponsesRequestBuilder::convertTools(const std::vector<Tool>& tools, const ResponsesCompat& compat) const {
    Json out = Json::array();
    for (const auto& tool : tools) {
        Json entry = Json::object();
        entry["type"] = "function";
        entry["name"] = tool.name;
        entry["description"] = tool.description;
        entry["parameters"] = tool.parameters;
        if (compat.supportsStrictMode) {
            entry["strict"] = false;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

std::string ResponsesRequestBuilder::clampCacheKey(const std::string& key) const {
    std::size_t points = 0;
    for (std::size_t i = 0; i < key.size(); ++i) {
        if ((static_cast<unsigned char>(key[i]) & 0xC0) != 0x80 && ++points > 64) {
            return key.substr(0, i);
        }
    }
    return key;
}

bool ResponsesRequestBuilder::isChatGptSignIn(const Model& model, const StreamOptions& options) const {
    return model.provider == "openai" && model.baseUrl == "https://api.openai.com/v1" && options.apiKey &&
           !options.apiKey->starts_with("sk-");
}

void ResponsesRequestBuilder::applyCache(Json& params, const ResponsesCompat& compat, const StreamOptions& options,
                                         bool omitUnsupported) const {
    const std::string mode = retention(options);
    if (mode != "none" && options.sessionId) {
        params["prompt_cache_key"] = clampCacheKey(*options.sessionId);
    }
    if (omitUnsupported) {
        return;
    }
    if (mode == "long" && compat.supportsLongCacheRetention && !compat.supportsExplicitPromptCacheMode) {
        params["prompt_cache_retention"] = "24h";
    }
    if (compat.supportsExplicitPromptCacheMode) {
        if (mode == "none") {
            params["prompt_cache_options"] = Json{{"mode", "explicit"}};
        } else if (mode == "long" && compat.supportsLongCacheRetention) {
            params["prompt_cache_options"] = Json{{"ttl", "30m"}};
        }
    }
}

void ResponsesRequestBuilder::applyReasoning(Json& params, const Model& model, const StreamOptions& options) const {
    if (!model.reasoning) {
        return;
    }
    std::optional<ThinkingLevel> effort;
    if (options.reasoning != ThinkingLevel::Off) {
        const ThinkingLevel clamped = m_levels.clamp(model, options.reasoning);
        if (clamped != ThinkingLevel::Off) {
            effort = clamped;
        }
    }
    if (effort) {
        const std::string name = m_levels.levelName(*effort);
        const auto entry = mapEntry(model, name);
        const std::string mapped = entry && entry->is_string() ? entry->get<std::string>() : name;
        params["reasoning"] = Json{{"effort", mapped}, {"summary", "auto"}};
        params["include"] = Json::array({"reasoning.encrypted_content"});
    } else if (model.provider != "github-copilot") {
        const auto off = mapEntry(model, "off");
        if (!off || !off->is_null()) {
            params["reasoning"] = Json{{"effort", off && off->is_string() ? off->get<std::string>() : "none"}};
        }
    }
    if (model.provider == "xai") {
        params["include"] = Json::array({"reasoning.encrypted_content"});
    }
}

Json ResponsesRequestBuilder::build(const Model& model, const TranscriptContext& rawContext,
                                    const StreamOptions& options, std::int64_t nowMs,
                                    const std::set<std::string>& toolCallProviders) const {
    const ResponsesCompat compat = m_compat.resolve(model);
    const TranscriptContext context = m_normalizer.resolveTranscript(rawContext, compat.supportsMidConvoSystemMessages);
    const bool omitUnsupported = isChatGptSignIn(model, options);
    Json params = Json::object();
    params["model"] = model.id;
    params["input"] = convertMessages(model, context, compat, toolCallProviders, true, nowMs);
    params["stream"] = true;
    params["store"] = false;
    applyCache(params, compat, options, omitUnsupported);
    const std::int64_t maxTokens =
        m_budgets.clampMaxTokensToContext(model, context, options.maxTokens.value_or(model.maxTokens));
    if (maxTokens > 0 && compat.supportsMaxOutputTokens && !omitUnsupported) {
        // OpenAI rejects max_output_tokens below 16.
        params["max_output_tokens"] = std::max<std::int64_t>(maxTokens, 16);
    }
    if (options.temperature && !omitUnsupported) {
        params["temperature"] = *options.temperature;
    }
    const std::vector<Tool> tools = m_normalizer.currentTools(context.messages);
    if (!tools.empty()) {
        params["tools"] = convertTools(tools, compat);
    }
    if (options.toolChoice) {
        params["tool_choice"] = *options.toolChoice;
    }
    applyReasoning(params, model, options);
    for (const Json* sampling : {&model.samplingParams, &options.samplingParams}) {
        if (sampling->is_object()) {
            for (const auto& entry : sampling->items()) {
                params[entry.key()] = entry.value();
            }
        }
    }
    return params;
}
