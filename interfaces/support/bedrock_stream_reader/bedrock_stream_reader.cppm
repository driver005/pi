module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.support.bedrock_stream_reader;

import std;
export import pi.platform.i_base64_codec;
export import pi.support.assistant_stream_emitter;
export import pi.support.cost_calculator;
export import pi.types.aws_event_stream_message;
export import pi.types.json;
export import pi.types.model;
export import pi.types.result;

/**
 * Turns Bedrock ConverseStream event messages into AssistantStreamEmitter calls: text, tool use
 * and reasoning blocks keyed by contentBlockIndex (including encrypted reasoning kept verbatim in
 * the thinking signature), stop reason, usage with cost, and the service's stream exceptions.
 * Port of the stream loop and handlers in api/bedrock-converse-stream.ts.
 */
export class BedrockStreamReader {
public:
    BedrockStreamReader(AssistantStreamEmitter& emitter, const Model& model, const IBase64Codec& base64);

    Result<void> handle(const AwsEventStreamMessage& message);

    /** Ends blocks that were never stopped and emits Done, or returns why the stream is unusable. */
    Result<void> finish();

private:
    Result<void> handleEvent(const std::string& type, const Json& payload);
    Result<void> handleException(const std::string& type, const Json& payload) const;
    void blockStart(const Json& event);
    void blockDelta(const Json& event);
    void textDelta(int position, const std::string& text);
    void toolDelta(int position, const std::string& input);
    void reasoningDelta(int position, const Json& reasoning);
    void redactedDelta(int blockIndex, const std::string& chunk);
    void blockStop(const Json& event);
    void closeBlock(int position, int blockIndex);
    void readUsage(const Json& usage);
    void applyStopReason(const std::string& reason);
    std::string flushRedacted(int blockIndex);
    std::int64_t numberIn(const Json& object, const std::string& key) const;
    int positionOf(const Json& event) const;

    AssistantStreamEmitter& m_emitter;
    Model m_model;
    const IBase64Codec& m_base64;
    CostCalculator m_cost;
    /** Bedrock's content block position to the emitter's block index. */
    std::map<int, int> m_blocks;
    std::map<int, std::vector<std::string>> m_redactedChunks;
};

BedrockStreamReader::BedrockStreamReader(AssistantStreamEmitter& emitter, const Model& model, const IBase64Codec& base64)
    : m_emitter(emitter), m_model(model), m_base64(base64) {}

std::int64_t BedrockStreamReader::numberIn(const Json& object, const std::string& key) const {
    return object.is_object() && object.contains(key) && object[key].is_number() ? object[key].get<std::int64_t>() : 0;
}

int BedrockStreamReader::positionOf(const Json& event) const {
    return event.contains("contentBlockIndex") && event["contentBlockIndex"].is_number_integer()
               ? event["contentBlockIndex"].get<int>()
               : 0;
}

Result<void> BedrockStreamReader::handle(const AwsEventStreamMessage& message) {
    const auto type = message.headers.find(":message-type");
    const Json payload = message.payload.empty() ? Json::object() : Json::parse(message.payload, nullptr, false);
    if (type != message.headers.end() && type->second == "exception") {
        const auto exception = message.headers.find(":exception-type");
        return handleException(exception == message.headers.end() ? "" : exception->second,
                               payload.is_discarded() ? Json::object() : payload);
    }
    const auto event = message.headers.find(":event-type");
    if (event == message.headers.end()) {
        return {};
    }
    if (payload.is_discarded()) {
        return std::unexpected(Error{"parse_error", "Could not parse Bedrock event: " + message.payload});
    }
    return handleEvent(event->second, payload);
}

Result<void> BedrockStreamReader::handleException(const std::string& type, const Json& payload) const {
    std::string prefix = type;
    if (type == "internalServerException") {
        prefix = "Internal server error";
    } else if (type == "modelStreamErrorException") {
        prefix = "Model stream error";
    } else if (type == "validationException") {
        prefix = "Validation error";
    } else if (type == "throttlingException") {
        prefix = "Throttling error";
    } else if (type == "serviceUnavailableException") {
        prefix = "Service unavailable";
    }
    const std::string text = payload.is_object() && payload.contains("message") && payload["message"].is_string()
                                 ? payload["message"].get<std::string>()
                                 : payload.dump();
    return std::unexpected(Error{type, prefix + ": " + text});
}

Result<void> BedrockStreamReader::handleEvent(const std::string& type, const Json& payload) {
    if (type == "messageStart") {
        if (payload.value("role", "") != "assistant") {
            return std::unexpected(
                Error{"unexpected_role", "Unexpected assistant message start but got user message start instead"});
        }
    } else if (type == "contentBlockStart") {
        blockStart(payload);
    } else if (type == "contentBlockDelta") {
        blockDelta(payload);
    } else if (type == "contentBlockStop") {
        blockStop(payload);
    } else if (type == "messageStop") {
        applyStopReason(payload.value("stopReason", ""));
    } else if (type == "metadata") {
        if (payload.contains("usage") && payload["usage"].is_object()) {
            readUsage(payload["usage"]);
        }
    }
    return {};
}

void BedrockStreamReader::blockStart(const Json& event) {
    if (!event.contains("start") || !event["start"].is_object() || !event["start"].contains("toolUse")) {
        return;
    }
    const Json& use = event["start"]["toolUse"];
    m_blocks[positionOf(event)] = m_emitter.toolCallStart(use.value("toolUseId", ""), use.value("name", ""));
}

void BedrockStreamReader::blockDelta(const Json& event) {
    if (!event.contains("delta") || !event["delta"].is_object()) {
        return;
    }
    const Json& delta = event["delta"];
    const int position = positionOf(event);
    if (delta.contains("text") && delta["text"].is_string()) {
        textDelta(position, delta["text"].get<std::string>());
    } else if (delta.contains("toolUse") && delta["toolUse"].is_object()) {
        toolDelta(position, delta["toolUse"].value("input", ""));
    } else if (delta.contains("reasoningContent") && delta["reasoningContent"].is_object()) {
        reasoningDelta(position, delta["reasoningContent"]);
    }
}

void BedrockStreamReader::textDelta(int position, const std::string& text) {
    if (!m_blocks.contains(position)) {
        m_blocks[position] = m_emitter.textStart();
    }
    const int index = m_blocks[position];
    if (std::holds_alternative<TextContent>(m_emitter.message().content[static_cast<std::size_t>(index)])) {
        m_emitter.textDelta(index, text);
    }
}

void BedrockStreamReader::toolDelta(int position, const std::string& input) {
    const auto found = m_blocks.find(position);
    if (found != m_blocks.end() &&
        std::holds_alternative<ToolCall>(m_emitter.message().content[static_cast<std::size_t>(found->second)])) {
        m_emitter.toolCallDelta(found->second, input);
    }
}

void BedrockStreamReader::reasoningDelta(int position, const Json& reasoning) {
    if (!m_blocks.contains(position)) {
        m_blocks[position] = m_emitter.thinkingStart();
    }
    const int index = m_blocks[position];
    auto* block = std::get_if<ThinkingContent>(&m_emitter.message().content[static_cast<std::size_t>(index)]);
    if (block == nullptr) {
        return;
    }
    if (reasoning.contains("text") && reasoning["text"].is_string() && !reasoning["text"].get<std::string>().empty()) {
        m_emitter.thinkingDelta(index, reasoning["text"].get<std::string>());
    }
    // The signature field holds an Anthropic signature or an opaque redacted payload, never both.
    if (reasoning.contains("signature") && reasoning["signature"].is_string() &&
        !reasoning["signature"].get<std::string>().empty() && !block->redacted.value_or(false)) {
        block->thinkingSignature = block->thinkingSignature.value_or("") + reasoning["signature"].get<std::string>();
    }
    if (reasoning.contains("redactedContent") && reasoning["redactedContent"].is_string() &&
        !reasoning["redactedContent"].get<std::string>().empty()) {
        redactedDelta(position, reasoning["redactedContent"].get<std::string>());
    }
}

void BedrockStreamReader::redactedDelta(int position, const std::string& chunk) {
    const int index = m_blocks[position];
    auto& block = std::get<ThinkingContent>(m_emitter.message().content[static_cast<std::size_t>(index)]);
    if (!block.redacted.value_or(false)) {
        block.redacted = true;
        block.thinkingSignature = "";
        m_emitter.thinkingDelta(index, "[Reasoning redacted]");
    }
    m_redactedChunks[position].push_back(chunk);
}

std::string BedrockStreamReader::flushRedacted(int position) {
    const auto chunks = m_redactedChunks.find(position);
    if (chunks == m_redactedChunks.end()) {
        return "";
    }
    std::string bytes;
    for (const auto& chunk : chunks->second) {
        if (const auto decoded = m_base64.decode(chunk)) {
            bytes += *decoded;
        }
    }
    m_redactedChunks.erase(chunks);
    return m_base64.encode(bytes);
}

void BedrockStreamReader::closeBlock(int position, int index) {
    auto& content = m_emitter.message().content[static_cast<std::size_t>(index)];
    if (std::holds_alternative<TextContent>(content)) {
        m_emitter.textEnd(index);
    } else if (auto* thinking = std::get_if<ThinkingContent>(&content)) {
        if (m_redactedChunks.contains(position)) {
            thinking->thinkingSignature = flushRedacted(position);
        }
        m_emitter.thinkingEnd(index);
    } else {
        m_emitter.toolCallEnd(index);
    }
}

void BedrockStreamReader::blockStop(const Json& event) {
    const int position = positionOf(event);
    const auto found = m_blocks.find(position);
    if (found == m_blocks.end()) {
        return;
    }
    closeBlock(position, found->second);
    m_blocks.erase(found);
}

void BedrockStreamReader::readUsage(const Json& usage) {
    Usage out;
    out.input = numberIn(usage, "inputTokens");
    out.output = numberIn(usage, "outputTokens");
    out.cacheRead = numberIn(usage, "cacheReadInputTokens");
    out.cacheWrite = numberIn(usage, "cacheWriteInputTokens");
    if (usage.contains("cacheDetails") && usage["cacheDetails"].is_array()) {
        std::int64_t oneHour = 0;
        for (const auto& detail : usage["cacheDetails"]) {
            oneHour += detail.value("ttl", "") == "1h" ? numberIn(detail, "inputTokens") : 0;
        }
        out.cacheWrite1h = oneHour;
    }
    out.totalTokens = numberIn(usage, "totalTokens");
    if (out.totalTokens == 0) {
        out.totalTokens = out.input + out.output;
    }
    m_cost.calculate(m_model, out);
    m_emitter.message().usage = out;
}

void BedrockStreamReader::applyStopReason(const std::string& reason) {
    AssistantMessage& out = m_emitter.message();
    out.rawStopReason = reason;
    if (reason == "end_turn" || reason == "stop_sequence") {
        out.stopReason = StopReason::Stop;
    } else if (reason == "max_tokens" || reason == "model_context_window_exceeded") {
        out.stopReason = StopReason::Length;
    } else if (reason == "tool_use") {
        out.stopReason = StopReason::ToolUse;
    } else {
        out.stopReason = StopReason::Error;
        if (!reason.empty()) {
            out.errorMessage = "Provider stopped with: " + reason;
        }
    }
}

Result<void> BedrockStreamReader::finish() {
    for (const auto& block : std::map<int, int>(m_blocks)) {
        closeBlock(block.first, block.second);
    }
    m_blocks.clear();
    AssistantMessage& out = m_emitter.message();
    if (out.stopReason == StopReason::Pending) {
        return std::unexpected(Error{"no_stop_reason", "Bedrock stream ended without a stop reason"});
    }
    if (out.stopReason == StopReason::Error || out.stopReason == StopReason::Aborted) {
        return std::unexpected(Error{"model_error", out.errorMessage.value_or("An unknown error occurred")});
    }
    m_emitter.done(out.stopReason);
    return {};
}
