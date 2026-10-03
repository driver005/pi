module;

#include <cstdint>

export module pi.support.bedrock_request_builder;

import std;
export import pi.platform.i_base64_codec;
export import pi.support.message_transformer;
export import pi.support.thinking_budget_calculator;
export import pi.support.thinking_level_resolver;
export import pi.support.transcript_normalizer;
export import pi.types.json;
export import pi.types.model;
export import pi.types.result;
export import pi.types.stream_options;
export import pi.types.transcript_context;

/**
 * Builds Bedrock ConverseStream request bodies (REST JSON). Port of the request side of
 * api/bedrock-converse-stream.ts, including the budget handling of streamSimple for budget-based
 * Claude models. Strict tool sampling and request metadata are not ported.
 */
export class BedrockRequestBuilder {
public:
    explicit BedrockRequestBuilder(const IBase64Codec& base64);

    /**
     * region is the configured AWS region (used only to omit Claude's thinking display field on
     * GovCloud). env holds the provider environment: PI_CACHE_RETENTION and AWS_BEDROCK_FORCE_CACHE.
     * An error when an image has a MIME type Bedrock does not know.
     */
    Result<Json> build(const Model& model, const TranscriptContext& context, const StreamOptions& options,
                       const std::string& region, std::int64_t nowMs) const;

    bool isAnthropicClaude(const Model& model) const;

private:
    std::string lower(const std::string& text) const;
    std::vector<std::string> candidates(const std::string& modelId, const std::string& modelName) const;
    bool anyContains(const std::vector<std::string>& values, const std::vector<std::string>& needles) const;
    bool supportsAdaptiveThinking(const Model& model) const;
    bool supportsNativeXhigh(const Model& model) const;
    bool supportsPromptCaching(const Model& model, const StreamOptions& options) const;
    std::string retention(const StreamOptions& options) const;
    Json cachePoint(const std::string& retention) const;
    bool isBlank(const std::string& text) const;
    Json textBlock(const std::string& text) const;
    std::string normalizeToolCallId(const std::string& id) const;
    Json sanitizeDocument(const Json& value) const;
    Result<Json> imageBlock(const ImageContent& image) const;
    Result<Json> toolResultContent(const std::vector<UserContentBlock>& content) const;
    Result<Json> convertMessages(const Model& model, const TranscriptContext& context, const StreamOptions& options,
                                 std::int64_t nowMs) const;
    Result<Json> userMessage(const UserMessage& message) const;
    Json assistantMessage(const Model& model, const AssistantMessage& message) const;
    Json assistantBlock(const Model& model, const AssistantContentBlock& block) const;
    Json thinkingBlock(const Model& model, const ThinkingContent& block) const;
    Json toolConfig(const std::vector<Tool>& tools, const std::optional<std::string>& choice) const;
    Json additionalFields(const Model& model, const StreamOptions& options, const std::string& region,
                          const Json& budgets) const;
    std::string effortFor(const Model& model, ThinkingLevel level) const;
    std::int64_t maxTokensFor(const Model& model, const TranscriptContext& context, const StreamOptions& options,
                              Json& budgets) const;

    const IBase64Codec& m_base64;
    TranscriptNormalizer m_normalizer;
    MessageTransformer m_transformer;
    ThinkingBudgetCalculator m_budgets;
    ThinkingLevelResolver m_levels;
};

BedrockRequestBuilder::BedrockRequestBuilder(const IBase64Codec& base64) : m_base64(base64) {}

std::string BedrockRequestBuilder::lower(const std::string& text) const {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool BedrockRequestBuilder::isBlank(const std::string& text) const {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::vector<std::string> BedrockRequestBuilder::candidates(const std::string& modelId, const std::string& modelName) const {
    std::vector<std::string> out;
    for (const std::string& value : {modelId, modelName}) {
        if (value.empty()) {
            continue;
        }
        const std::string lowered = lower(value);
        std::string dashed;
        bool inRun = false;
        for (const char c : lowered) {
            if (std::isspace(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == ':') {
                if (!inRun) {
                    dashed.push_back('-');
                }
                inRun = true;
            } else {
                dashed.push_back(c);
                inRun = false;
            }
        }
        out.push_back(lowered);
        out.push_back(dashed);
    }
    return out;
}

bool BedrockRequestBuilder::anyContains(const std::vector<std::string>& values, const std::vector<std::string>& needles) const {
    for (const auto& value : values) {
        for (const auto& needle : needles) {
            if (value.find(needle) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

bool BedrockRequestBuilder::isAnthropicClaude(const Model& model) const {
    const std::string id = lower(model.id);
    const std::string name = lower(model.name);
    return id.find("anthropic.claude") != std::string::npos || id.find("anthropic/claude") != std::string::npos ||
           name.find("anthropic.claude") != std::string::npos || name.find("anthropic/claude") != std::string::npos ||
           name.find("claude") != std::string::npos;
}

bool BedrockRequestBuilder::supportsAdaptiveThinking(const Model& model) const {
    return anyContains(candidates(model.id, model.name), {"opus-4-6", "opus-4-7", "opus-4-8", "opus-5", "sonnet-4-6",
                                                         "sonnet-5", "fable-5"});
}

bool BedrockRequestBuilder::supportsNativeXhigh(const Model& model) const {
    return anyContains(candidates(model.id, model.name), {"opus-4-7", "opus-4-8", "opus-5", "sonnet-5", "fable-5"});
}

bool BedrockRequestBuilder::supportsPromptCaching(const Model& model, const StreamOptions& options) const {
    const auto names = candidates(model.id, model.name);
    if (!anyContains(names, {"claude"})) {
        const auto force = options.env.find("AWS_BEDROCK_FORCE_CACHE");
        return force != options.env.end() && force->second == "1";
    }
    return anyContains(names, {"fable-5", "opus-5", "sonnet-5", "-4-", "claude-3-7-sonnet", "claude-3-5-haiku"});
}

std::string BedrockRequestBuilder::retention(const StreamOptions& options) const {
    if (options.cacheRetention) {
        return *options.cacheRetention;
    }
    const auto env = options.env.find("PI_CACHE_RETENTION");
    return env != options.env.end() && env->second == "long" ? "long" : "short";
}

Json BedrockRequestBuilder::cachePoint(const std::string& retention) const {
    Json point = Json{{"type", "default"}};
    if (retention == "long") {
        point["ttl"] = "1h";
    }
    return Json{{"cachePoint", std::move(point)}};
}

Json BedrockRequestBuilder::textBlock(const std::string& text) const {
    return isBlank(text) ? Json() : Json{{"text", text}};
}

std::string BedrockRequestBuilder::normalizeToolCallId(const std::string& id) const {
    std::string out;
    for (const char c : id) {
        out.push_back(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' ? c : '_');
    }
    if (out.size() > 64) {
        out.resize(64);
    }
    return out;
}

Json BedrockRequestBuilder::sanitizeDocument(const Json& value) const {
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& item : value) {
            out.push_back(sanitizeDocument(item));
        }
        return out;
    }
    if (value.is_object()) {
        Json out = Json::object();
        for (const auto& entry : value.items()) {
            if (!entry.key().empty()) {
                out[entry.key()] = sanitizeDocument(entry.value());
            }
        }
        return out;
    }
    return value;
}

Result<Json> BedrockRequestBuilder::imageBlock(const ImageContent& image) const {
    std::string format;
    if (image.mimeType == "image/jpeg" || image.mimeType == "image/jpg") {
        format = "jpeg";
    } else if (image.mimeType == "image/png") {
        format = "png";
    } else if (image.mimeType == "image/gif") {
        format = "gif";
    } else if (image.mimeType == "image/webp") {
        format = "webp";
    } else {
        return std::unexpected(Error{"unknown_image_type", "Unknown image type: " + image.mimeType});
    }
    return Json{{"image", Json{{"format", format}, {"source", Json{{"bytes", image.data}}}}}};
}

Result<Json> BedrockRequestBuilder::toolResultContent(const std::vector<UserContentBlock>& content) const {
    Json out = Json::array();
    for (const auto& block : content) {
        if (const auto* image = std::get_if<ImageContent>(&block)) {
            auto converted = imageBlock(*image);
            if (!converted) {
                return std::unexpected(converted.error());
            }
            out.push_back(std::move(*converted));
        } else if (Json text = textBlock(std::get<TextContent>(block).text); !text.is_null()) {
            out.push_back(std::move(text));
        }
    }
    if (out.empty()) {
        out.push_back(Json{{"text", "<empty>"}});
    }
    return out;
}

Result<Json> BedrockRequestBuilder::userMessage(const UserMessage& message) const {
    Json content = Json::array();
    if (const auto* text = std::get_if<std::string>(&message.content)) {
        Json block = textBlock(*text);
        content.push_back(block.is_null() ? Json{{"text", "<empty>"}} : block);
    } else {
        for (const auto& item : std::get<std::vector<UserContentBlock>>(message.content)) {
            if (const auto* image = std::get_if<ImageContent>(&item)) {
                auto converted = imageBlock(*image);
                if (!converted) {
                    return std::unexpected(converted.error());
                }
                content.push_back(std::move(*converted));
            } else if (Json block = textBlock(std::get<TextContent>(item).text); !block.is_null()) {
                content.push_back(std::move(block));
            }
        }
        if (content.empty()) {
            content.push_back(Json{{"text", "<empty>"}});
        }
    }
    return Json{{"role", "user"}, {"content", std::move(content)}};
}

Json BedrockRequestBuilder::thinkingBlock(const Model& model, const ThinkingContent& block) const {
    if (block.redacted.value_or(false)) {
        // Opaque encrypted reasoning is replayed as redactedContent; a payload that is not base64 is dropped.
        if (block.thinkingSignature && !block.thinkingSignature->empty() && m_base64.decode(*block.thinkingSignature)) {
            return Json{{"reasoningContent", Json{{"redactedContent", *block.thinkingSignature}}}};
        }
        return Json();
    }
    if (isBlank(block.thinking)) {
        return Json();
    }
    if (!isAnthropicClaude(model)) {
        return Json{{"reasoningContent", Json{{"reasoningText", Json{{"text", block.thinking}}}}}};
    }
    if (!block.thinkingSignature || isBlank(*block.thinkingSignature)) {
        return Json{{"text", block.thinking}};
    }
    return Json{{"reasoningContent",
                 Json{{"reasoningText", Json{{"text", block.thinking}, {"signature", *block.thinkingSignature}}}}}};
}

Json BedrockRequestBuilder::assistantBlock(const Model& model, const AssistantContentBlock& block) const {
    if (const auto* text = std::get_if<TextContent>(&block)) {
        return textBlock(text->text);
    }
    if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
        return thinkingBlock(model, *thinking);
    }
    const auto& call = std::get<ToolCall>(block);
    return Json{{"toolUse", Json{{"toolUseId", call.id}, {"name", call.name}, {"input", sanitizeDocument(call.arguments)}}}};
}

Json BedrockRequestBuilder::assistantMessage(const Model& model, const AssistantMessage& message) const {
    Json content = Json::array();
    for (const auto& block : message.content) {
        Json converted = assistantBlock(model, block);
        if (!converted.is_null()) {
            content.push_back(std::move(converted));
        }
    }
    return content.empty() ? Json() : Json{{"role", "assistant"}, {"content", std::move(content)}};
}

Result<Json> BedrockRequestBuilder::convertMessages(const Model& model, const TranscriptContext& context,
                                                    const StreamOptions& options, std::int64_t nowMs) const {
    TranscriptContext conversation = m_normalizer.collapseSystemMessages(context);
    if (!conversation.messages.empty() && std::holds_alternative<SystemMessage>(conversation.messages.front())) {
        conversation.messages.erase(conversation.messages.begin());
    }
    const auto transformed = m_transformer.transform(
        conversation.messages, model,
        [&](const std::string& id, const Model&, const AssistantMessage&) { return normalizeToolCallId(id); }, nowMs);
    Json out = Json::array();
    for (std::size_t i = 0; i < transformed.size(); ++i) {
        if (const auto* user = std::get_if<UserMessage>(&transformed[i])) {
            auto message = userMessage(*user);
            if (!message) {
                return std::unexpected(message.error());
            }
            out.push_back(std::move(*message));
        } else if (const auto* assistant = std::get_if<AssistantMessage>(&transformed[i])) {
            Json message = assistantMessage(model, *assistant);
            if (!message.is_null()) {
                out.push_back(std::move(message));
            }
        } else if (std::holds_alternative<ToolResultMessage>(transformed[i])) {
            // Bedrock needs all consecutive tool results in one user message.
            Json results = Json::array();
            std::size_t j = i;
            for (; j < transformed.size() && std::holds_alternative<ToolResultMessage>(transformed[j]); ++j) {
                const auto& result = std::get<ToolResultMessage>(transformed[j]);
                auto content = toolResultContent(result.content);
                if (!content) {
                    return std::unexpected(content.error());
                }
                results.push_back(Json{{"toolResult", Json{{"toolUseId", result.toolCallId},
                                                           {"content", std::move(*content)},
                                                           {"status", result.isError ? "error" : "success"}}}});
            }
            i = j - 1;
            out.push_back(Json{{"role", "user"}, {"content", std::move(results)}});
        }
    }
    if (retention(options) != "none" && supportsPromptCaching(model, options) && !out.empty() &&
        out.back()["role"] == "user") {
        out.back()["content"].push_back(cachePoint(retention(options)));
    }
    return out;
}

Json BedrockRequestBuilder::toolConfig(const std::vector<Tool>& tools, const std::optional<std::string>& choice) const {
    if (tools.empty() || (choice && *choice == "none")) {
        return Json();
    }
    Json specs = Json::array();
    for (const auto& tool : tools) {
        specs.push_back(Json{{"toolSpec", Json{{"name", tool.name},
                                               {"description", tool.description},
                                               {"inputSchema", Json{{"json", tool.parameters}}}}}});
    }
    Json config = Json{{"tools", std::move(specs)}};
    if (choice && *choice == "auto") {
        config["toolChoice"] = Json{{"auto", Json::object()}};
    } else if (choice && *choice == "any") {
        config["toolChoice"] = Json{{"any", Json::object()}};
    }
    return config;
}

std::string BedrockRequestBuilder::effortFor(const Model& model, ThinkingLevel level) const {
    if (level == ThinkingLevel::XHigh && supportsNativeXhigh(model)) {
        return "xhigh";
    }
    const std::string name = m_levels.levelName(level);
    if (model.thinkingLevelMap.is_object() && model.thinkingLevelMap.contains(name) &&
        model.thinkingLevelMap[name].is_string()) {
        return model.thinkingLevelMap[name].get<std::string>();
    }
    if (level == ThinkingLevel::Minimal || level == ThinkingLevel::Low) {
        return "low";
    }
    return level == ThinkingLevel::Medium ? "medium" : "high";
}

std::int64_t BedrockRequestBuilder::maxTokensFor(const Model& model, const TranscriptContext& context,
                                                 const StreamOptions& options, Json& budgets) const {
    budgets = options.thinkingBudgets.is_object() ? options.thinkingBudgets : Json::object();
    const bool budgetClaude = options.reasoning != ThinkingLevel::Off && isAnthropicClaude(model) &&
                              !supportsAdaptiveThinking(model);
    if (!budgetClaude) {
        return m_budgets.clampMaxTokensToContext(model, context, options.maxTokens.value_or(model.maxTokens));
    }
    const auto adjusted = m_budgets.adjustMaxTokens(options.maxTokens, model.maxTokens, options.reasoning, budgets);
    const std::int64_t maxTokens = m_budgets.clampMaxTokensToContext(model, context, adjusted.maxTokens);
    const ThinkingLevel keyLevel = options.reasoning == ThinkingLevel::XHigh || options.reasoning == ThinkingLevel::Max
                                       ? ThinkingLevel::High
                                       : options.reasoning;
    budgets[m_levels.levelName(keyLevel)] =
        std::min(adjusted.thinkingBudget, std::max<std::int64_t>(0, maxTokens - 1024));
    return maxTokens;
}

Json BedrockRequestBuilder::additionalFields(const Model& model, const StreamOptions& options,
                                             const std::string& region, const Json& budgets) const {
    if (options.reasoning == ThinkingLevel::Off || !model.reasoning || !isAnthropicClaude(model)) {
        return Json();
    }
    const std::string modelId = lower(model.id);
    const bool govCloud = lower(region).starts_with("us-gov-") || modelId.starts_with("us-gov.") ||
                          modelId.starts_with("arn:aws-us-gov:");
    Json fields = Json::object();
    const bool adaptive = supportsAdaptiveThinking(model);
    if (adaptive) {
        Json thinking = Json{{"type", "adaptive"}};
        if (!govCloud) {
            thinking["display"] = "summarized";
        }
        fields["thinking"] = std::move(thinking);
        fields["output_config"] = Json{{"effort", effortFor(model, options.reasoning)}};
        return fields;
    }
    const std::map<ThinkingLevel, std::int64_t> defaults{{ThinkingLevel::Minimal, 1024}, {ThinkingLevel::Low, 2048},
                                                         {ThinkingLevel::Medium, 8192}, {ThinkingLevel::High, 16384},
                                                         {ThinkingLevel::XHigh, 16384}, {ThinkingLevel::Max, 16384}};
    const ThinkingLevel keyLevel = options.reasoning == ThinkingLevel::XHigh || options.reasoning == ThinkingLevel::Max
                                       ? ThinkingLevel::High
                                       : options.reasoning;
    const std::string key = m_levels.levelName(keyLevel);
    const std::int64_t budget = budgets.is_object() && budgets.contains(key) && budgets[key].is_number_integer()
                                    ? budgets[key].get<std::int64_t>()
                                    : defaults.at(options.reasoning);
    Json thinking = Json{{"type", "enabled"}, {"budget_tokens", budget}};
    if (!govCloud) {
        thinking["display"] = "summarized";
    }
    fields["thinking"] = std::move(thinking);
    fields["anthropic_beta"] = Json::array({"interleaved-thinking-2025-05-14"});
    return fields;
}

Result<Json> BedrockRequestBuilder::build(const Model& model, const TranscriptContext& rawContext,
                                          const StreamOptions& options, const std::string& region,
                                          std::int64_t nowMs) const {
    const TranscriptContext context = m_normalizer.collapseSystemMessages(rawContext);
    auto messages = convertMessages(model, context, options, nowMs);
    if (!messages) {
        return std::unexpected(messages.error());
    }
    Json body = Json::object();
    body["messages"] = std::move(*messages);
    if (const SystemMessage* system = m_normalizer.initialSystemMessage(context.messages)) {
        const std::string text = m_normalizer.systemMessageText(*system);
        if (!text.empty()) {
            Json blocks = Json::array({Json{{"text", text}}});
            if (retention(options) != "none" && supportsPromptCaching(model, options)) {
                blocks.push_back(cachePoint(retention(options)));
            }
            body["system"] = std::move(blocks);
        }
    }
    Json budgets;
    Json inference = Json::object();
    inference["maxTokens"] = maxTokensFor(model, context, options, budgets);
    if (options.temperature) {
        inference["temperature"] = *options.temperature;
    }
    body["inferenceConfig"] = std::move(inference);
    Json tools = toolConfig(m_normalizer.currentTools(context.messages), options.toolChoice);
    if (!tools.is_null()) {
        body["toolConfig"] = std::move(tools);
    }
    Json extra = additionalFields(model, options, region, budgets);
    if (!extra.is_null()) {
        body["additionalModelRequestFields"] = std::move(extra);
    }
    return body;
}
