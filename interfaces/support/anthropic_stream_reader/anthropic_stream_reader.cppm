module;

#include <cstdint>

export module pi.support.anthropic_stream_reader;

import std;
export import pi.support.assistant_stream_emitter;
export import pi.support.cost_calculator;
export import pi.support.partial_json_parser;
export import pi.types.model;
export import pi.types.result;
export import pi.types.sse_event;
export import pi.types.tool;

/**
 * Turns Anthropic Messages SSE events into AssistantStreamEmitter calls: content blocks,
 * usage with cost, stop reason. The emitter must already have emitted start(). Port of the event
 * loop and mapStopReason in packages/ai/src/api/anthropic-messages.ts.
 */
export class AnthropicStreamReader {
public:
    /**
     * isOAuth maps Claude Code tool names back to the caller's casing using tools.
     */
    AnthropicStreamReader(AssistantStreamEmitter& emitter, const Model& model, bool isOAuth,
                          std::vector<Tool> tools);

    /** Handles one SSE event. Error means the stream must be abandoned. */
    Result<void> handle(const SseEvent& event);

    /** Validates the stream ended cleanly and emits the terminal Done event. */
    Result<void> finish();

private:
    Result<void> handleMessageStart(const Json& event);
    Result<void> handleBlockStart(const Json& event);
    Result<void> handleBlockDelta(const Json& event);
    Result<void> handleBlockStop(const Json& event);
    Result<void> handleMessageDelta(const Json& event);

    std::string restoreToolName(const std::string& name) const;
    void startToolUse(int anthropicIndex, const Json& block);
    void startThinking(int anthropicIndex, const Json& block);
    void startText(int anthropicIndex, const Json& block);
    std::optional<int> contentIndex(const Json& event) const;
    void recomputeUsage();
    void readStartUsage(const Json& usage);
    void readDeltaUsage(const Json& usage);
    Result<StopReason> mapStopReason(const std::string& reason, const Json& details,
                                     std::string& errorMessage) const;
    std::int64_t intField(const Json& object, const std::string& key) const;
    bool hasNumber(const Json& object, const std::string& key) const;

    AssistantStreamEmitter& m_emitter;
    Model m_model;
    Model m_usageModel;
    bool m_isOAuth;
    std::vector<Tool> m_tools;
    PartialJsonParser m_json;
    CostCalculator m_cost;
    std::map<int, int> m_blockIndex;
    bool m_sawStart = false;
    bool m_sawStop = false;
};

AnthropicStreamReader::AnthropicStreamReader(AssistantStreamEmitter& emitter, const Model& model,
                                             bool isOAuth, std::vector<Tool> tools)
    : m_emitter(emitter),
      m_model(model),
      m_usageModel(model),
      m_isOAuth(isOAuth),
      m_tools(std::move(tools)) {}

bool AnthropicStreamReader::hasNumber(const Json& object, const std::string& key) const {
    return object.is_object() && object.contains(key) && object[key].is_number();
}

std::int64_t AnthropicStreamReader::intField(const Json& object, const std::string& key) const {
    return hasNumber(object, key) ? object[key].get<std::int64_t>() : 0;
}

Result<void> AnthropicStreamReader::handle(const SseEvent& sse) {
    if (sse.event == "error") {
        return std::unexpected(Error{"stream_error", sse.data});
    }
    const std::set<std::string> known = {"message_start",       "message_delta",
                                         "message_stop",        "content_block_start",
                                         "content_block_delta", "content_block_stop"};
    if (!known.contains(sse.event)) {
        return {};
    }
    auto parsed = m_json.parseWithRepair(sse.data);
    if (!parsed.has_value()) {
        return std::unexpected(Error{"parse_error", "Could not parse Anthropic SSE event " +
                                                        sse.event + ": " + parsed.error().message +
                                                        "; data=" + sse.data});
    }
    const Json& event = *parsed;
    const std::string type = event.value("type", "");
    if (type == "message_start") {
        return handleMessageStart(event);
    }
    if (type == "content_block_start") {
        return handleBlockStart(event);
    }
    if (type == "content_block_delta") {
        return handleBlockDelta(event);
    }
    if (type == "content_block_stop") {
        return handleBlockStop(event);
    }
    if (type == "message_delta") {
        return handleMessageDelta(event);
    }
    if (type == "message_stop") {
        m_sawStop = true;
    }
    return {};
}

void AnthropicStreamReader::recomputeUsage() {
    Usage& usage = m_emitter.message().usage;
    usage.totalTokens = usage.input + usage.output + usage.cacheRead + usage.cacheWrite;
    m_cost.calculate(m_usageModel, usage);
}

void AnthropicStreamReader::readStartUsage(const Json& usage) {
    Usage& out = m_emitter.message().usage;
    out.input = intField(usage, "input_tokens");
    out.output = intField(usage, "output_tokens");
    out.cacheRead = intField(usage, "cache_read_input_tokens");
    out.cacheWrite = intField(usage, "cache_creation_input_tokens");
    const Json creation = usage.is_object() && usage.contains("cache_creation") ? usage["cache_creation"] : Json();
    out.cacheWrite1h = intField(creation, "ephemeral_1h_input_tokens");
}

Result<void> AnthropicStreamReader::handleMessageStart(const Json& event) {
    m_sawStart = true;
    const Json message = event.contains("message") ? event["message"] : Json::object();
    AssistantMessage& out = m_emitter.message();
    if (message.contains("id") && message["id"].is_string()) {
        out.responseId = message["id"].get<std::string>();
    }
    const std::string responseModel = message.value("model", m_model.id);
    if (responseModel != m_model.id) {
        out.responseModel = responseModel;
    }
    if (message.contains("usage")) {
        readStartUsage(message["usage"]);
    }
    recomputeUsage();
    return {};
}

std::optional<int> AnthropicStreamReader::contentIndex(const Json& event) const {
    if (!hasNumber(event, "index")) {
        return std::nullopt;
    }
    const auto found = m_blockIndex.find(event["index"].get<int>());
    if (found == m_blockIndex.end()) {
        return std::nullopt;
    }
    return found->second;
}

void AnthropicStreamReader::startText(int anthropicIndex, const Json& block) {
    const int index = m_emitter.textStart();
    m_blockIndex[anthropicIndex] = index;
    if (block.contains("text") && block["text"].is_string()) {
        std::get<TextContent>(m_emitter.message().content[static_cast<std::size_t>(index)]).text =
            block["text"].get<std::string>();
    }
}

void AnthropicStreamReader::startThinking(int anthropicIndex, const Json& block) {
    const bool redacted = block.value("type", "") == "redacted_thinking";
    const int index = m_emitter.thinkingStart();
    m_blockIndex[anthropicIndex] = index;
    auto& thinking =
        std::get<ThinkingContent>(m_emitter.message().content[static_cast<std::size_t>(index)]);
    if (redacted) {
        thinking.thinking = "[Reasoning redacted]";
        thinking.thinkingSignature = block.value("data", "");
        thinking.redacted = true;
        return;
    }
    thinking.thinking = block.value("thinking", "");
    thinking.thinkingSignature = block.value("signature", "");
}

std::string AnthropicStreamReader::restoreToolName(const std::string& name) const {
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const std::string wanted = lower(name);
    for (const auto& tool : m_tools) {
        if (lower(tool.name) == wanted) {
            return tool.name;
        }
    }
    return name;
}

void AnthropicStreamReader::startToolUse(int anthropicIndex, const Json& block) {
    const std::string name = block.value("name", "");
    const int index =
        m_emitter.toolCallStart(block.value("id", ""), m_isOAuth ? restoreToolName(name) : name);
    m_blockIndex[anthropicIndex] = index;
    if (block.contains("input") && block["input"].is_object() && !block["input"].empty()) {
        std::get<ToolCall>(m_emitter.message().content[static_cast<std::size_t>(index)]).arguments =
            block["input"];
    }
}

Result<void> AnthropicStreamReader::handleBlockStart(const Json& event) {
    if (!event.contains("content_block") || !hasNumber(event, "index")) {
        return {};
    }
    const Json& block = event["content_block"];
    const std::string type = block.value("type", "");
    const int anthropicIndex = event["index"].get<int>();
    if (type == "fallback") {
        if (!m_emitter.message().content.empty()) {
            return std::unexpected(
                Error{"unsupported_fallback", "Anthropic performed an unsupported mid-output model fallback"});
        }
    } else if (type == "text") {
        startText(anthropicIndex, block);
    } else if (type == "thinking" || type == "redacted_thinking") {
        startThinking(anthropicIndex, block);
    } else if (type == "tool_use") {
        startToolUse(anthropicIndex, block);
    }
    return {};
}

Result<void> AnthropicStreamReader::handleBlockDelta(const Json& event) {
    const auto index = contentIndex(event);
    if (!index || !event.contains("delta")) {
        return {};
    }
    const Json& delta = event["delta"];
    const std::string type = delta.value("type", "");
    auto& block = m_emitter.message().content[static_cast<std::size_t>(*index)];
    if (type == "text_delta" && std::holds_alternative<TextContent>(block)) {
        m_emitter.textDelta(*index, delta.value("text", ""));
    } else if (type == "thinking_delta" && std::holds_alternative<ThinkingContent>(block)) {
        m_emitter.thinkingDelta(*index, delta.value("thinking", ""));
    } else if (type == "input_json_delta" && std::holds_alternative<ToolCall>(block)) {
        m_emitter.toolCallDelta(*index, delta.value("partial_json", ""));
    } else if (type == "signature_delta" && std::holds_alternative<ThinkingContent>(block)) {
        auto& thinking = std::get<ThinkingContent>(block);
        thinking.thinkingSignature = thinking.thinkingSignature.value_or("") + delta.value("signature", "");
    }
    return {};
}

Result<void> AnthropicStreamReader::handleBlockStop(const Json& event) {
    const auto index = contentIndex(event);
    if (!index) {
        return {};
    }
    const auto& block = m_emitter.message().content[static_cast<std::size_t>(*index)];
    if (std::holds_alternative<TextContent>(block)) {
        m_emitter.textEnd(*index);
    } else if (std::holds_alternative<ThinkingContent>(block)) {
        m_emitter.thinkingEnd(*index);
    } else {
        m_emitter.toolCallEnd(*index);
    }
    return {};
}

Result<StopReason> AnthropicStreamReader::mapStopReason(const std::string& reason,
                                                        const Json& details,
                                                        std::string& errorMessage) const {
    if (reason == "end_turn" || reason == "pause_turn" || reason == "stop_sequence") {
        return StopReason::Stop;
    }
    if (reason == "max_tokens") {
        return StopReason::Length;
    }
    if (reason == "tool_use") {
        return StopReason::ToolUse;
    }
    if (reason == "refusal") {
        const std::string explanation = details.is_object() ? details.value("explanation", "") : "";
        errorMessage = explanation.empty() ? "The model refused to complete the request" : explanation;
        return StopReason::Error;
    }
    if (reason == "sensitive") {
        errorMessage = "Provider stopped with: sensitive";
        return StopReason::Error;
    }
    return std::unexpected(Error{"unhandled_stop_reason", "Unhandled stop reason: " + reason});
}

void AnthropicStreamReader::readDeltaUsage(const Json& usage) {
    Usage& out = m_emitter.message().usage;
    if (hasNumber(usage, "input_tokens")) {
        out.input = usage["input_tokens"].get<std::int64_t>();
    }
    if (hasNumber(usage, "output_tokens")) {
        out.output = usage["output_tokens"].get<std::int64_t>();
    }
    if (hasNumber(usage, "cache_read_input_tokens")) {
        out.cacheRead = usage["cache_read_input_tokens"].get<std::int64_t>();
    }
    if (hasNumber(usage, "cache_creation_input_tokens")) {
        out.cacheWrite = usage["cache_creation_input_tokens"].get<std::int64_t>();
    }
    if (usage.contains("cache_creation") && hasNumber(usage["cache_creation"], "ephemeral_1h_input_tokens")) {
        out.cacheWrite1h = usage["cache_creation"]["ephemeral_1h_input_tokens"].get<std::int64_t>();
    }
    if (usage.contains("output_tokens_details") &&
        hasNumber(usage["output_tokens_details"], "thinking_tokens")) {
        out.reasoning = usage["output_tokens_details"]["thinking_tokens"].get<std::int64_t>();
    }
}

Result<void> AnthropicStreamReader::handleMessageDelta(const Json& event) {
    AssistantMessage& out = m_emitter.message();
    const Json delta = event.contains("delta") ? event["delta"] : Json::object();
    if (delta.contains("stop_reason") && delta["stop_reason"].is_string()) {
        const std::string raw = delta["stop_reason"].get<std::string>();
        out.rawStopReason = raw;
        std::string errorMessage;
        auto reason = mapStopReason(raw, delta.contains("stop_details") ? delta["stop_details"] : Json(),
                                    errorMessage);
        if (!reason) {
            return std::unexpected(reason.error());
        }
        out.stopReason = *reason;
        if (!errorMessage.empty()) {
            out.errorMessage = errorMessage;
        }
    }
    if (event.contains("usage") && event["usage"].is_object()) {
        readDeltaUsage(event["usage"]);
    }
    recomputeUsage();
    return {};
}

Result<void> AnthropicStreamReader::finish() {
    if (m_sawStart && !m_sawStop) {
        return std::unexpected(Error{"stream_truncated", "Anthropic stream ended before message_stop"});
    }
    AssistantMessage& out = m_emitter.message();
    if (out.stopReason == StopReason::Pending) {
        return std::unexpected(Error{"no_stop_reason", "Anthropic stream ended without a stop reason"});
    }
    if (out.stopReason == StopReason::Error || out.stopReason == StopReason::Aborted) {
        return std::unexpected(Error{"model_error", out.errorMessage.value_or("An unknown error occurred")});
    }
    m_emitter.done(out.stopReason);
    return {};
}
