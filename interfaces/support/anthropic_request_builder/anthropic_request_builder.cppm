module;

#include <cstdint>

export module pi.support.anthropic_request_builder;

import std;
export import pi.support.message_transformer;
export import pi.support.thinking_budget_calculator;
export import pi.support.thinking_level_resolver;
export import pi.support.transcript_normalizer;
export import pi.types.anthropic_thinking_plan;
export import pi.types.json;
export import pi.types.model;
export import pi.types.stream_options;
export import pi.types.transcript_context;

/**
 * Builds the JSON body and beta list of an Anthropic Messages request from a transcript.
 * Port of buildParams/convertMessages/convertTools/getBetaFeatures and the streamSimple
 * reasoning mapping in packages/ai/src/api/anthropic-messages.ts.
 *
 * Not ported: native mid-conversation tool changes (inline-tools beta), managed effort
 * models, server-side fallbacks and strict tool schemas.
 */
export class AnthropicRequestBuilder {
public:
    /** The request body without the `betas` key (those travel in the anthropic-beta header). */
    Json build(const Model& model, const TranscriptContext& context, bool isOAuth,
               const StreamOptions& options, std::int64_t nowMs) const;

    std::vector<std::string> betaFeatures(const Model& model, const TranscriptContext& context,
                                          bool isOAuth, const StreamOptions& options) const;

    /** Reasoning level -> thinking parameters and response ceiling. */
    AnthropicThinkingPlan planThinking(const Model& model, const TranscriptContext& context,
                                       const StreamOptions& options) const;

    bool isOAuthToken(const std::string& apiKey) const;

    /** Maps a Claude Code canonical tool name back to the caller's casing. */
    std::string fromClaudeCodeName(const std::string& name, const std::vector<Tool>& tools) const;

private:
    bool compatBool(const Model& model, const std::string& key, bool fallback) const;
    std::string lower(const std::string& text) const;
    bool isBlank(const std::string& text) const;

    std::string retention(const StreamOptions& options) const;
    Json cacheControl(const Model& model, const StreamOptions& options) const;
    std::string toClaudeCodeName(const std::string& name) const;
    std::string toolName(const std::string& name, bool isOAuth) const;

    Json textBlock(const std::string& text) const;
    Json imageBlock(const ImageContent& image) const;
    Json toolResultContent(const std::vector<UserContentBlock>& content) const;
    Json toolResultBlock(const ToolResultMessage& message) const;
    std::optional<Json> userMessage(const UserMessage& message) const;
    std::optional<Json> assistantMessage(const AssistantMessage& message, bool isOAuth,
                                         bool allowEmptySignature) const;
    Json thinkingBlock(const ThinkingContent& block, bool allowEmptySignature) const;
    std::vector<Json> convertMessages(const std::vector<Message>& messages, bool isOAuth,
                                      const Json& cache, bool allowEmptySignature) const;
    void addMessageCacheControl(std::vector<Json>& messages, const Json& cache) const;

    Json convertTools(const std::vector<Tool>& tools, bool isOAuth, bool eagerStreaming,
                      const Json& cache) const;
    Json buildSystem(const std::string& text, bool isOAuth, const Json& cache) const;
    void applyThinking(Json& params, const Model& model, const AnthropicThinkingPlan& plan) const;
    std::string mapEffort(const Model& model, ThinkingLevel level) const;
    std::vector<std::string> splitBetas(const std::string& value) const;

    TranscriptNormalizer m_normalizer;
    MessageTransformer m_transformer;
    ThinkingBudgetCalculator m_budgets;
    ThinkingLevelResolver m_levels;
};

bool AnthropicRequestBuilder::compatBool(const Model& model, const std::string& key,
                                         bool fallback) const {
    if (model.compat.is_object() && model.compat.contains(key) && model.compat[key].is_boolean()) {
        return model.compat[key].get<bool>();
    }
    return fallback;
}

std::string AnthropicRequestBuilder::lower(const std::string& text) const {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool AnthropicRequestBuilder::isBlank(const std::string& text) const {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool AnthropicRequestBuilder::isOAuthToken(const std::string& apiKey) const {
    return apiKey.find("sk-ant-oat") != std::string::npos;
}

std::string AnthropicRequestBuilder::retention(const StreamOptions& options) const {
    if (options.cacheRetention) {
        return *options.cacheRetention;
    }
    const auto env = options.env.find("PI_CACHE_RETENTION");
    return env != options.env.end() && env->second == "long" ? "long" : "short";
}

Json AnthropicRequestBuilder::cacheControl(const Model& model, const StreamOptions& options) const {
    const std::string mode = retention(options);
    if (mode == "none") {
        return Json();
    }
    Json control = Json::object();
    control["type"] = "ephemeral";
    if (mode == "long" && compatBool(model, "supportsLongCacheRetention", true)) {
        control["ttl"] = "1h";
    }
    return control;
}

std::string AnthropicRequestBuilder::toClaudeCodeName(const std::string& name) const {
    const std::vector<std::string> canonical = {
        "Read",       "Write",        "Edit",         "Bash",      "Grep",      "Glob",
        "AskUserQuestion", "EnterPlanMode", "ExitPlanMode", "KillShell", "NotebookEdit", "Skill",
        "Task",       "TaskOutput",   "TodoWrite",    "WebFetch",  "WebSearch"};
    const std::string wanted = lower(name);
    for (const auto& candidate : canonical) {
        if (lower(candidate) == wanted) {
            return candidate;
        }
    }
    return name;
}

std::string AnthropicRequestBuilder::toolName(const std::string& name, bool isOAuth) const {
    return isOAuth ? toClaudeCodeName(name) : name;
}

std::string AnthropicRequestBuilder::fromClaudeCodeName(const std::string& name,
                                                        const std::vector<Tool>& tools) const {
    const std::string wanted = lower(name);
    for (const auto& tool : tools) {
        if (lower(tool.name) == wanted) {
            return tool.name;
        }
    }
    return name;
}

Json AnthropicRequestBuilder::textBlock(const std::string& text) const {
    Json block = Json::object();
    block["type"] = "text";
    block["text"] = text;
    return block;
}

Json AnthropicRequestBuilder::imageBlock(const ImageContent& image) const {
    Json source = Json::object();
    source["type"] = "base64";
    source["media_type"] = image.mimeType;
    source["data"] = image.data;
    Json block = Json::object();
    block["type"] = "image";
    block["source"] = std::move(source);
    return block;
}

Json AnthropicRequestBuilder::toolResultContent(const std::vector<UserContentBlock>& content) const {
    bool hasImages = false;
    bool hasText = false;
    for (const auto& block : content) {
        hasImages = hasImages || std::holds_alternative<ImageContent>(block);
        hasText = hasText || std::holds_alternative<TextContent>(block);
    }
    if (!hasImages) {
        std::string joined;
        for (std::size_t i = 0; i < content.size(); ++i) {
            joined += (i > 0 ? "\n" : "") + std::get<TextContent>(content[i]).text;
        }
        return joined;
    }
    Json blocks = Json::array();
    if (!hasText) {
        blocks.push_back(textBlock("(see attached image)"));
    }
    for (const auto& block : content) {
        if (const auto* text = std::get_if<TextContent>(&block)) {
            blocks.push_back(textBlock(text->text));
        } else {
            blocks.push_back(imageBlock(std::get<ImageContent>(block)));
        }
    }
    return blocks;
}

Json AnthropicRequestBuilder::toolResultBlock(const ToolResultMessage& message) const {
    Json block = Json::object();
    block["type"] = "tool_result";
    block["tool_use_id"] = message.toolCallId;
    block["content"] = toolResultContent(message.content);
    block["is_error"] = message.isError;
    return block;
}

std::optional<Json> AnthropicRequestBuilder::userMessage(const UserMessage& message) const {
    Json out = Json::object();
    out["role"] = "user";
    if (const auto* text = std::get_if<std::string>(&message.content)) {
        if (isBlank(*text)) {
            return std::nullopt;
        }
        out["content"] = *text;
        return out;
    }
    Json blocks = Json::array();
    for (const auto& block : std::get<std::vector<UserContentBlock>>(message.content)) {
        if (const auto* text = std::get_if<TextContent>(&block)) {
            if (!isBlank(text->text)) {
                blocks.push_back(textBlock(text->text));
            }
        } else {
            blocks.push_back(imageBlock(std::get<ImageContent>(block)));
        }
    }
    if (blocks.empty()) {
        return std::nullopt;
    }
    out["content"] = std::move(blocks);
    return out;
}

Json AnthropicRequestBuilder::thinkingBlock(const ThinkingContent& block,
                                            bool allowEmptySignature) const {
    Json out = Json::object();
    const bool signed_ = block.thinkingSignature && !isBlank(*block.thinkingSignature);
    if (signed_) {
        out["type"] = "thinking";
        out["thinking"] = block.thinking;
        out["signature"] = *block.thinkingSignature;
    } else if (allowEmptySignature) {
        out["type"] = "thinking";
        out["thinking"] = block.thinking;
        out["signature"] = "";
    } else {
        // An aborted stream leaves no signature; plain text is the only valid replay.
        out = textBlock(block.thinking);
    }
    return out;
}

std::optional<Json> AnthropicRequestBuilder::assistantMessage(const AssistantMessage& message,
                                                              bool isOAuth,
                                                              bool allowEmptySignature) const {
    Json blocks = Json::array();
    for (const auto& block : message.content) {
        if (const auto* text = std::get_if<TextContent>(&block)) {
            if (!isBlank(text->text)) {
                blocks.push_back(textBlock(text->text));
            }
        } else if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
            if (thinking->redacted.value_or(false)) {
                Json redacted = Json::object();
                redacted["type"] = "redacted_thinking";
                redacted["data"] = thinking->thinkingSignature.value_or("");
                blocks.push_back(std::move(redacted));
                continue;
            }
            const bool hasSignature =
                thinking->thinkingSignature && !isBlank(*thinking->thinkingSignature);
            if (isBlank(thinking->thinking) && !hasSignature) {
                continue;
            }
            blocks.push_back(thinkingBlock(*thinking, allowEmptySignature));
        } else {
            const auto& call = std::get<ToolCall>(block);
            Json use = Json::object();
            use["type"] = "tool_use";
            use["id"] = call.id;
            use["name"] = toolName(call.name, isOAuth);
            use["input"] = call.arguments.is_null() ? Json::object() : call.arguments;
            blocks.push_back(std::move(use));
        }
    }
    if (blocks.empty()) {
        return std::nullopt;
    }
    Json out = Json::object();
    out["role"] = "assistant";
    out["content"] = std::move(blocks);
    return out;
}

std::vector<Json> AnthropicRequestBuilder::convertMessages(const std::vector<Message>& messages,
                                                           bool isOAuth, const Json& cache,
                                                           bool allowEmptySignature) const {
    std::vector<Json> params;
    std::vector<Json> pendingSystem;
    auto flush = [&]() {
        params.insert(params.end(), pendingSystem.begin(), pendingSystem.end());
        pendingSystem.clear();
    };
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const Message& message = messages[i];
        if (const auto* system = std::get_if<SystemMessage>(&message)) {
            const std::string text = m_normalizer.renderSystemMessageUpdate(*system);
            if (!text.empty()) {
                Json out = Json::object();
                out["role"] = "system";
                out["content"] = Json::array({textBlock(text)});
                pendingSystem.push_back(std::move(out));
            }
        } else if (const auto* user = std::get_if<UserMessage>(&message)) {
            if (auto converted = userMessage(*user)) {
                params.push_back(std::move(*converted));
            }
        } else if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            flush();
            if (auto converted = assistantMessage(*assistant, isOAuth, allowEmptySignature)) {
                params.push_back(std::move(*converted));
            }
        } else {
            // Consecutive tool results share one user message (needed by z.ai-style endpoints).
            Json results = Json::array();
            std::size_t j = i;
            while (j < messages.size() && std::holds_alternative<ToolResultMessage>(messages[j])) {
                results.push_back(toolResultBlock(std::get<ToolResultMessage>(messages[j])));
                ++j;
            }
            i = j - 1;
            Json out = Json::object();
            out["role"] = "user";
            out["content"] = std::move(results);
            params.push_back(std::move(out));
        }
    }
    flush();
    addMessageCacheControl(params, cache);
    return params;
}

void AnthropicRequestBuilder::addMessageCacheControl(std::vector<Json>& messages,
                                                     const Json& cache) const {
    if (cache.is_null() || messages.empty()) {
        return;
    }
    Json& last = messages.back();
    const std::string role = last["role"].get<std::string>();
    if (role != "user" && role != "system") {
        return;
    }
    Json& content = last["content"];
    if (content.is_string()) {
        Json block = textBlock(content.get<std::string>());
        block["cache_control"] = cache;
        content = Json::array({std::move(block)});
        return;
    }
    if (!content.is_array() || content.empty()) {
        return;
    }
    Json& block = content.back();
    const std::string type = block.value("type", "");
    if (type == "text" || type == "image" || type == "tool_result") {
        block["cache_control"] = cache;
    }
}

Json AnthropicRequestBuilder::convertTools(const std::vector<Tool>& tools, bool isOAuth,
                                           bool eagerStreaming, const Json& cache) const {
    Json out = Json::array();
    for (std::size_t i = 0; i < tools.size(); ++i) {
        const Tool& tool = tools[i];
        Json schema = Json::object();
        schema["type"] = "object";
        const Json& parameters = tool.parameters;
        schema["properties"] = parameters.is_object() && parameters.contains("properties")
                                   ? parameters["properties"]
                                   : Json::object();
        schema["required"] = parameters.is_object() && parameters.contains("required")
                                 ? parameters["required"]
                                 : Json::array();
        Json entry = Json::object();
        entry["name"] = toolName(tool.name, isOAuth);
        entry["description"] = tool.description;
        if (eagerStreaming) {
            entry["eager_input_streaming"] = true;
        }
        entry["input_schema"] = std::move(schema);
        if (!cache.is_null() && i + 1 == tools.size()) {
            entry["cache_control"] = cache;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

Json AnthropicRequestBuilder::buildSystem(const std::string& text, bool isOAuth,
                                          const Json& cache) const {
    Json blocks = Json::array();
    auto add = [&](const std::string& value) {
        Json block = textBlock(value);
        if (!cache.is_null()) {
            block["cache_control"] = cache;
        }
        blocks.push_back(std::move(block));
    };
    if (isOAuth) {
        add("You are Claude Code, Anthropic's official CLI for Claude.");
    }
    if (!text.empty()) {
        add(text);
    }
    return blocks;
}

std::string AnthropicRequestBuilder::mapEffort(const Model& model, ThinkingLevel level) const {
    const std::string name = m_levels.levelName(level);
    if (model.thinkingLevelMap.is_object() && model.thinkingLevelMap.contains(name) &&
        model.thinkingLevelMap[name].is_string()) {
        return model.thinkingLevelMap[name].get<std::string>();
    }
    switch (level) {
        case ThinkingLevel::Minimal:
        case ThinkingLevel::Low: return "low";
        case ThinkingLevel::Medium: return "medium";
        default: return "high";
    }
}

AnthropicThinkingPlan AnthropicRequestBuilder::planThinking(const Model& model,
                                                            const TranscriptContext& context,
                                                            const StreamOptions& options) const {
    AnthropicThinkingPlan plan;
    const std::int64_t requested = options.maxTokens.value_or(model.maxTokens);
    const std::int64_t base = m_budgets.clampMaxTokensToContext(model, context, requested);
    plan.maxTokens = base;
    if (options.reasoning == ThinkingLevel::Off) {
        return plan;
    }
    plan.enabled = true;
    if (compatBool(model, "forceAdaptiveThinking", false)) {
        plan.effort = mapEffort(model, options.reasoning);
        return plan;
    }
    const auto adjusted =
        m_budgets.adjustMaxTokens(base, model.maxTokens, options.reasoning, options.thinkingBudgets);
    plan.maxTokens = m_budgets.clampMaxTokensToContext(model, context, adjusted.maxTokens);
    plan.budgetTokens =
        std::min(adjusted.thinkingBudget, std::max<std::int64_t>(0, plan.maxTokens - 1024));
    return plan;
}

void AnthropicRequestBuilder::applyThinking(Json& params, const Model& model,
                                            const AnthropicThinkingPlan& plan) const {
    if (!model.reasoning) {
        return;
    }
    Json thinking = Json::object();
    if (plan.enabled) {
        thinking["type"] = plan.effort ? "adaptive" : "enabled";
        if (!plan.effort) {
            thinking["budget_tokens"] = plan.budgetTokens.value_or(0) != 0 ? *plan.budgetTokens : 1024;
        }
        thinking["display"] = "summarized";
        params["thinking"] = std::move(thinking);
        if (plan.effort) {
            Json output = Json::object();
            output["effort"] = *plan.effort;
            params["output_config"] = std::move(output);
        }
        return;
    }
    const bool offHidden = model.thinkingLevelMap.is_object() &&
                           model.thinkingLevelMap.contains("off") &&
                           model.thinkingLevelMap["off"].is_null();
    if (!offHidden) {
        thinking["type"] = "disabled";
        params["thinking"] = std::move(thinking);
    }
}

Json AnthropicRequestBuilder::build(const Model& model, const TranscriptContext& rawContext,
                                    bool isOAuth, const StreamOptions& options,
                                    std::int64_t nowMs) const {
    const TranscriptContext context = m_normalizer.resolveTranscript(
        rawContext, compatBool(model, "supportsMidConvoSystemMessages", false));
    const Json cache = cacheControl(model, options);
    const SystemMessage* initial = m_normalizer.initialSystemMessage(context.messages);
    const std::string systemText = initial ? m_normalizer.systemMessageText(*initial) : "";

    const auto normalizeId = [this](const std::string& id, const Model&, const AssistantMessage&) {
        return m_transformer.sanitizeToolCallId(id, 64);
    };
    std::vector<Message> transformed = m_transformer.transform(context.messages, model, normalizeId, nowMs);
    if (initial && !transformed.empty()) {
        transformed.erase(transformed.begin());
    }
    const AnthropicThinkingPlan plan = planThinking(model, context, options);

    Json params = Json::object();
    params["model"] = model.id;
    params["messages"] = convertMessages(transformed, isOAuth, cache,
                                         compatBool(model, "allowEmptySignature", false));
    params["max_tokens"] = plan.maxTokens;
    params["stream"] = true;
    if (isOAuth || !systemText.empty()) {
        params["system"] = buildSystem(systemText, isOAuth, cache);
    }
    if (options.temperature && !plan.enabled && compatBool(model, "supportsTemperature", true)) {
        params["temperature"] = *options.temperature;
    }
    const std::vector<Tool> tools = m_normalizer.currentTools(context.messages);
    if (!tools.empty()) {
        const Json toolCache = compatBool(model, "supportsCacheControlOnTools", true) ? cache : Json();
        params["tools"] = convertTools(tools, isOAuth,
                                       compatBool(model, "supportsEagerToolInputStreaming", true),
                                       toolCache);
    }
    applyThinking(params, model, plan);
    if (options.metadata.is_object() && options.metadata.contains("user_id") &&
        options.metadata["user_id"].is_string()) {
        Json metadata = Json::object();
        metadata["user_id"] = options.metadata["user_id"];
        params["metadata"] = std::move(metadata);
    }
    if (options.toolChoice) {
        Json choice = Json::object();
        choice["type"] = *options.toolChoice;
        params["tool_choice"] = std::move(choice);
    }
    return params;
}

std::vector<std::string> AnthropicRequestBuilder::splitBetas(const std::string& value) const {
    std::vector<std::string> out;
    std::string current;
    auto flush = [&]() {
        const auto first = current.find_first_not_of(" \t");
        if (first != std::string::npos) {
            const auto last = current.find_last_not_of(" \t");
            const std::string feature = current.substr(first, last - first + 1);
            if (std::find(out.begin(), out.end(), feature) == out.end()) {
                out.push_back(feature);
            }
        }
        current.clear();
    };
    for (const char c : value) {
        if (c == ',') {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();
    return out;
}

std::vector<std::string> AnthropicRequestBuilder::betaFeatures(const Model& model,
                                                               const TranscriptContext& context,
                                                               bool isOAuth,
                                                               const StreamOptions& options) const {
    std::optional<std::string> configured;
    bool suppressed = false;
    for (const auto& entry : model.headers) {
        if (lower(entry.first) == "anthropic-beta") {
            configured = entry.second;
            suppressed = false;
        }
    }
    for (const auto& entry : options.headers) {
        if (lower(entry.first) == "anthropic-beta") {
            configured = entry.second;
            suppressed = !entry.second.has_value();
        }
    }
    if (suppressed) {
        return {};
    }
    if (configured) {
        return splitBetas(*configured);
    }
    std::vector<std::string> features;
    if (isOAuth) {
        features.push_back("claude-code-20250219");
        features.push_back("oauth-2025-04-20");
    }
    const bool hasTools = !m_normalizer.currentTools(context.messages).empty();
    if (hasTools && !compatBool(model, "supportsEagerToolInputStreaming", true)) {
        features.push_back("fine-grained-tool-streaming-2025-05-14");
    }
    if (model.reasoning && options.reasoning != ThinkingLevel::Off &&
        !compatBool(model, "forceAdaptiveThinking", false)) {
        features.push_back("interleaved-thinking-2025-05-14");
    }
    return features;
}
