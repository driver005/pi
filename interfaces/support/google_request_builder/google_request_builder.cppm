module;

#include <cstdint>

export module pi.support.google_request_builder;

import std;
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
 * Builds Gemini generateContent request bodies (REST JSON), shared by the Google Generative AI
 * and Vertex AI providers. Port of google-shared.ts plus buildParams / getGoogleBudget in
 * google-generative-ai.ts. Strict (validated) tool sampling is not ported.
 */
export class GoogleRequestBuilder {
public:
    /**
     * The request body; an error when the model's thinking level mapping is unusable. Vertex has
     * no separate thinking budgets for flash-lite models (liteBudgets false).
     */
    Result<Json> build(const Model& model, const TranscriptContext& context, const StreamOptions& options,
                       std::int64_t nowMs, bool liteBudgets = true) const;

    Json convertMessages(const Model& model, const TranscriptContext& context, std::int64_t nowMs) const;
    Json convertTools(const std::vector<Tool>& tools) const;

    /** Gemini 3+, Claude and gpt-oss models behind Google APIs need explicit tool call ids. */
    bool requiresToolCallId(const std::string& modelId) const;
    /** Whether the model takes a discrete thinkingLevel instead of a token budget. */
    bool usesThinkingLevel(const Model& model) const;

private:
    std::optional<int> geminiMajorVersion(const std::string& modelId) const;
    bool supportsMultimodalFunctionResponse(const std::string& modelId) const;
    std::string lower(const std::string& text) const;
    bool isBlank(const std::string& text) const;
    bool validSignature(const std::optional<std::string>& signature) const;
    std::optional<std::string> keptSignature(bool sameModel, const std::optional<std::string>& signature) const;
    std::string normalizeToolCallId(const Model& model, const std::string& id) const;
    Json inlineImage(const ImageContent& image) const;
    void appendUser(const UserMessage& message, Json& contents) const;
    void appendAssistant(const Model& model, const AssistantMessage& message, Json& contents) const;
    Json assistantPart(const Model& model, const AssistantContentBlock& block, bool sameModel) const;
    void appendToolResult(const Model& model, const ToolResultMessage& message, Json& contents) const;
    Result<Json> thinkingConfig(const Model& model, const StreamOptions& options, bool liteBudgets) const;
    Result<std::string> googleLevel(const Model& model, ThinkingLevel level) const;
    Json disabledThinking(const Model& model) const;
    std::int64_t budgetFor(const Model& model, const std::string& level, const Json& custom, bool liteBudgets) const;
    std::string functionCallingMode(const std::optional<std::string>& toolChoice) const;

    TranscriptNormalizer m_normalizer;
    MessageTransformer m_transformer;
    ThinkingBudgetCalculator m_budgets;
    ThinkingLevelResolver m_levels;
};

std::string GoogleRequestBuilder::lower(const std::string& text) const {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool GoogleRequestBuilder::isBlank(const std::string& text) const {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::optional<int> GoogleRequestBuilder::geminiMajorVersion(const std::string& modelId) const {
    const std::string id = lower(modelId);
    std::size_t pos = 0;
    if (id.starts_with("gemini-live-")) {
        pos = 12;
    } else if (id.starts_with("gemini-")) {
        pos = 7;
    } else {
        return std::nullopt;
    }
    std::size_t end = pos;
    while (end < id.size() && std::isdigit(static_cast<unsigned char>(id[end])) != 0) {
        ++end;
    }
    if (end == pos) {
        return std::nullopt;
    }
    return std::stoi(id.substr(pos, end - pos));
}

bool GoogleRequestBuilder::requiresToolCallId(const std::string& modelId) const {
    const auto major = geminiMajorVersion(modelId);
    return modelId.starts_with("claude-") || modelId.starts_with("gpt-oss-") || (major && *major >= 3);
}

bool GoogleRequestBuilder::supportsMultimodalFunctionResponse(const std::string& modelId) const {
    const auto major = geminiMajorVersion(modelId);
    return !major || *major >= 3;
}

bool GoogleRequestBuilder::usesThinkingLevel(const Model& model) const {
    const std::string id = lower(model.id);
    const auto gemini3 = [&](const std::string& tier) {
        const auto at = id.find("gemini-3");
        if (at == std::string::npos) {
            return false;
        }
        std::size_t pos = at + 8;
        if (pos < id.size() && id[pos] == '.') {
            ++pos;
            while (pos < id.size() && std::isdigit(static_cast<unsigned char>(id[pos])) != 0) {
                ++pos;
            }
        }
        return id.compare(pos, tier.size() + 1, "-" + tier) == 0;
    };
    return gemini3("pro") || gemini3("flash") || id == "gemini-flash-latest" ||
           id == "gemini-flash-lite-latest" || id.find("gemma-4") != std::string::npos ||
           id.find("gemma4") != std::string::npos;
}

bool GoogleRequestBuilder::validSignature(const std::optional<std::string>& signature) const {
    if (!signature || signature->empty() || signature->size() % 4 != 0) {
        return false;
    }
    std::size_t padding = 0;
    for (std::size_t i = 0; i < signature->size(); ++i) {
        const char c = (*signature)[i];
        if (c == '=') {
            ++padding;
        } else if (padding > 0 || (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '+' && c != '/')) {
            return false;
        }
    }
    return padding <= 2;
}

std::optional<std::string> GoogleRequestBuilder::keptSignature(bool sameModel,
                                                               const std::optional<std::string>& signature) const {
    return sameModel && validSignature(signature) ? signature : std::nullopt;
}

std::string GoogleRequestBuilder::normalizeToolCallId(const Model& model, const std::string& id) const {
    if (!requiresToolCallId(model.id)) {
        return id;
    }
    std::string out;
    for (const char c : id) {
        out.push_back(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' ? c : '_');
    }
    if (out.size() > 64) {
        out.resize(64);
    }
    return out;
}

Json GoogleRequestBuilder::inlineImage(const ImageContent& image) const {
    return Json{{"inlineData", Json{{"mimeType", image.mimeType}, {"data", image.data}}}};
}

void GoogleRequestBuilder::appendUser(const UserMessage& message, Json& contents) const {
    Json parts = Json::array();
    if (const auto* text = std::get_if<std::string>(&message.content)) {
        parts.push_back(Json{{"text", *text}});
    } else {
        for (const auto& block : std::get<std::vector<UserContentBlock>>(message.content)) {
            if (const auto* text = std::get_if<TextContent>(&block)) {
                parts.push_back(Json{{"text", text->text}});
            } else {
                parts.push_back(inlineImage(std::get<ImageContent>(block)));
            }
        }
    }
    if (!parts.empty()) {
        contents.push_back(Json{{"role", "user"}, {"parts", std::move(parts)}});
    }
}

Json GoogleRequestBuilder::assistantPart(const Model& model, const AssistantContentBlock& block,
                                         bool sameModel) const {
    if (const auto* text = std::get_if<TextContent>(&block)) {
        const auto signature = keptSignature(sameModel, text->textSignature);
        if (isBlank(text->text) && !signature) {
            return Json();
        }
        Json part = Json{{"text", text->text}};
        if (signature) {
            part["thoughtSignature"] = *signature;
        }
        return part;
    }
    if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
        if (!sameModel) {
            return isBlank(thinking->thinking) ? Json() : Json{{"text", thinking->thinking}};
        }
        const auto signature = keptSignature(sameModel, thinking->thinkingSignature);
        if (isBlank(thinking->thinking) && !signature) {
            return Json();
        }
        Json part = Json{{"thought", true}, {"text", thinking->thinking}};
        if (signature) {
            part["thoughtSignature"] = *signature;
        }
        return part;
    }
    const auto& call = std::get<ToolCall>(block);
    Json function = Json{{"name", call.name}, {"args", call.arguments.is_object() ? call.arguments : Json::object()}};
    if (requiresToolCallId(model.id)) {
        function["id"] = call.id;
    }
    Json part = Json{{"functionCall", std::move(function)}};
    if (const auto signature = keptSignature(sameModel, call.thoughtSignature)) {
        part["thoughtSignature"] = *signature;
    }
    return part;
}

void GoogleRequestBuilder::appendAssistant(const Model& model, const AssistantMessage& message,
                                           Json& contents) const {
    const bool sameModel = message.provider == model.provider && message.model == model.id;
    Json parts = Json::array();
    for (const auto& block : message.content) {
        Json part = assistantPart(model, block, sameModel);
        if (!part.is_null()) {
            parts.push_back(std::move(part));
        }
    }
    if (!parts.empty()) {
        contents.push_back(Json{{"role", "model"}, {"parts", std::move(parts)}});
    }
}

void GoogleRequestBuilder::appendToolResult(const Model& model, const ToolResultMessage& message,
                                            Json& contents) const {
    std::string text;
    bool first = true;
    Json images = Json::array();
    const bool modelSeesImages = std::find(model.input.begin(), model.input.end(), "image") != model.input.end();
    for (const auto& block : message.content) {
        if (const auto* part = std::get_if<TextContent>(&block)) {
            text += (first ? "" : "\n") + part->text;
            first = false;
        } else if (modelSeesImages) {
            images.push_back(inlineImage(std::get<ImageContent>(block)));
        }
    }
    const bool hasImages = !images.empty();
    const bool nested = supportsMultimodalFunctionResponse(model.id);
    const std::string value = !text.empty() ? text : hasImages ? "(see attached image)" : "";
    Json function = Json::object();
    function["name"] = message.toolName;
    function["response"] = message.isError ? Json{{"error", value}} : Json{{"output", value}};
    if (hasImages && nested) {
        function["parts"] = images;
    }
    if (requiresToolCallId(model.id)) {
        function["id"] = message.toolCallId;
    }
    Json part = Json{{"functionResponse", std::move(function)}};
    // All function responses of one round go into a single user turn.
    if (!contents.empty() && contents.back()["role"] == "user") {
        bool hasResponse = false;
        for (const auto& existing : contents.back()["parts"]) {
            hasResponse = hasResponse || existing.contains("functionResponse");
        }
        if (hasResponse) {
            contents.back()["parts"].push_back(std::move(part));
            part = Json();
        }
    }
    if (!part.is_null()) {
        contents.push_back(Json{{"role", "user"}, {"parts", Json::array({std::move(part)})}});
    }
    if (hasImages && !nested) {
        Json parts = Json::array({Json{{"text", "Tool result image:"}}});
        for (const auto& image : images) {
            parts.push_back(image);
        }
        contents.push_back(Json{{"role", "user"}, {"parts", std::move(parts)}});
    }
}

Json GoogleRequestBuilder::convertMessages(const Model& model, const TranscriptContext& context,
                                           std::int64_t nowMs) const {
    // Gemini has no mid-conversation system messages; the leading prompt is the systemInstruction.
    TranscriptContext conversation = m_normalizer.collapseSystemMessages(context);
    if (!conversation.messages.empty() && std::holds_alternative<SystemMessage>(conversation.messages.front())) {
        conversation.messages.erase(conversation.messages.begin());
    }
    const auto transformed = m_transformer.transform(
        conversation.messages, model,
        [&](const std::string& id, const Model&, const AssistantMessage&) { return normalizeToolCallId(model, id); },
        nowMs);
    Json contents = Json::array();
    for (const auto& message : transformed) {
        if (const auto* user = std::get_if<UserMessage>(&message)) {
            appendUser(*user, contents);
        } else if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            appendAssistant(model, *assistant, contents);
        } else if (const auto* result = std::get_if<ToolResultMessage>(&message)) {
            appendToolResult(model, *result, contents);
        }
    }
    return contents;
}

Json GoogleRequestBuilder::convertTools(const std::vector<Tool>& tools) const {
    Json declarations = Json::array();
    for (const auto& tool : tools) {
        declarations.push_back(Json{{"name", tool.name},
                                    {"description", tool.description},
                                    {"parametersJsonSchema", tool.parameters}});
    }
    return Json::array({Json{{"functionDeclarations", std::move(declarations)}}});
}

std::string GoogleRequestBuilder::functionCallingMode(const std::optional<std::string>& toolChoice) const {
    if (!toolChoice) {
        return "";
    }
    if (*toolChoice == "none") {
        return "NONE";
    }
    return *toolChoice == "any" ? "ANY" : "AUTO";
}

Result<std::string> GoogleRequestBuilder::googleLevel(const Model& model, ThinkingLevel level) const {
    const std::string name = m_levels.levelName(level);
    std::string resolved = name;
    if (model.thinkingLevelMap.is_object() && model.thinkingLevelMap.contains(name) &&
        model.thinkingLevelMap[name].is_string()) {
        resolved = lower(model.thinkingLevelMap[name].get<std::string>());
    }
    if (resolved == "minimal" || resolved == "low" || resolved == "medium" || resolved == "high") {
        std::string upper = resolved;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return upper;
    }
    return std::unexpected(Error{"unsupported_thinking_level",
                                 "Unsupported Google thinking level mapping for " + model.provider + "/" + model.id +
                                     ": " + name + " -> " + resolved});
}

Json GoogleRequestBuilder::disabledThinking(const Model& model) const {
    if (!usesThinkingLevel(model)) {
        return Json{{"thinkingBudget", 0}};
    }
    const ThinkingLevel fallback = m_levels.clamp(model, ThinkingLevel::Off);
    if (fallback == ThinkingLevel::Off) {
        return Json{{"thinkingBudget", 0}};
    }
    const auto level = googleLevel(model, fallback);
    return level ? Json{{"thinkingLevel", *level}} : Json{{"thinkingBudget", 0}};
}

std::int64_t GoogleRequestBuilder::budgetFor(const Model& model, const std::string& level, const Json& custom,
                                             bool liteBudgets) const {
    if (custom.is_object() && custom.contains(level) && custom[level].is_number_integer()) {
        return custom[level].get<std::int64_t>();
    }
    const std::map<std::string, std::int64_t>* budgets = nullptr;
    const std::map<std::string, std::int64_t> pro{{"minimal", 128}, {"low", 2048}, {"medium", 8192}, {"high", 32768}};
    const std::map<std::string, std::int64_t> lite{{"minimal", 512}, {"low", 2048}, {"medium", 8192}, {"high", 24576}};
    const std::map<std::string, std::int64_t> flash{{"minimal", 128}, {"low", 2048}, {"medium", 8192}, {"high", 24576}};
    if (model.id.find("2.5-pro") != std::string::npos) {
        budgets = &pro;
    } else if (liteBudgets && model.id.find("2.5-flash-lite") != std::string::npos) {
        budgets = &lite;
    } else if (model.id.find("2.5-flash") != std::string::npos) {
        budgets = &flash;
    }
    return budgets != nullptr ? budgets->at(level) : -1;
}

Result<Json> GoogleRequestBuilder::thinkingConfig(const Model& model, const StreamOptions& options,
                                                  bool liteBudgets) const {
    if (!model.reasoning) {
        return Json();
    }
    ThinkingLevel clamped = ThinkingLevel::Off;
    if (options.reasoning != ThinkingLevel::Off) {
        clamped = m_levels.clamp(model, options.reasoning);
    }
    if (clamped == ThinkingLevel::Off) {
        return disabledThinking(model);
    }
    const auto level = googleLevel(model, clamped);
    if (!level) {
        return std::unexpected(level.error());
    }
    Json config = Json{{"includeThoughts", true}};
    if (usesThinkingLevel(model)) {
        config["thinkingLevel"] = *level;
    } else {
        std::string lowered = lower(*level);
        config["thinkingBudget"] = budgetFor(model, lowered, options.thinkingBudgets, liteBudgets);
    }
    return config;
}

Result<Json> GoogleRequestBuilder::build(const Model& model, const TranscriptContext& rawContext,
                                         const StreamOptions& options, std::int64_t nowMs, bool liteBudgets) const {
    const TranscriptContext context = m_normalizer.collapseSystemMessages(rawContext);
    Json body = Json::object();
    body["contents"] = convertMessages(model, context, nowMs);
    if (const SystemMessage* system = m_normalizer.initialSystemMessage(context.messages)) {
        const std::string text = m_normalizer.systemMessageText(*system);
        if (!text.empty()) {
            body["systemInstruction"] = Json{{"role", "user"}, {"parts", Json::array({Json{{"text", text}}})}};
        }
    }
    const std::vector<Tool> tools = m_normalizer.currentTools(context.messages);
    if (!tools.empty()) {
        body["tools"] = convertTools(tools);
        const std::string mode = functionCallingMode(options.toolChoice);
        if (!mode.empty()) {
            body["toolConfig"] = Json{{"functionCallingConfig", Json{{"mode", mode}}}};
        }
    }
    Json generation = Json::object();
    if (options.temperature) {
        generation["temperature"] = *options.temperature;
    }
    const std::int64_t maxTokens =
        m_budgets.clampMaxTokensToContext(model, context, options.maxTokens.value_or(model.maxTokens));
    if (maxTokens > 0) {
        generation["maxOutputTokens"] = maxTokens;
    }
    auto thinking = thinkingConfig(model, options, liteBudgets);
    if (!thinking) {
        return std::unexpected(thinking.error());
    }
    if (!thinking->is_null()) {
        generation["thinkingConfig"] = std::move(*thinking);
    }
    if (!generation.empty()) {
        body["generationConfig"] = std::move(generation);
    }
    return body;
}
