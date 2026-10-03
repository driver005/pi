module;

#include <cstdint>

export module pi.support.google_stream_reader;

import std;
export import pi.platform.i_clock;
export import pi.support.assistant_stream_emitter;
export import pi.support.cost_calculator;
export import pi.types.json;
export import pi.types.model;
export import pi.types.result;
export import pi.types.sse_event;

/**
 * Turns Gemini streamGenerateContent chunks (one JSON object per SSE event) into
 * AssistantStreamEmitter calls: text and thought parts, function calls, usage with cost and the
 * finish reason. Shared by Google Generative AI and Vertex AI. The emitter must already have
 * emitted start(). Port of the chunk loop in api/google-generative-ai.ts.
 */
export class GoogleStreamReader {
public:
    GoogleStreamReader(AssistantStreamEmitter& emitter, const Model& model, const IClock& clock)
        : m_emitter(emitter),
          m_model(model),
          m_clock(clock) {}

    Result<void> handle(const SseEvent& sse) {
        if (sse.data.empty() || sse.data == "[DONE]") {
            return {};
        }
        const Json chunk = Json::parse(sse.data, nullptr, false);
        if (chunk.is_discarded()) {
            return std::unexpected(Error{"parse_error", "Could not parse Gemini chunk: " + sse.data});
        }
        if (!chunk.is_object()) {
            return {};
        }
        if (chunk.contains("error") && chunk["error"].is_object()) {
            const std::string message = stringField(chunk["error"], "message");
            return std::unexpected(
                Error{"provider_error", message.empty() ? chunk["error"].dump(-1, ' ', false, Json::error_handler_t::replace)
                                                        : message});
        }
        handleChunk(chunk);
        return {};
    }

    /** Ends the open block and emits Done, or returns why the stream is unusable. */
    Result<void> finish() {
        closeBlock();
        AssistantMessage& out = m_emitter.message();
        if (out.stopReason == StopReason::Pending) {
            return std::unexpected(Error{"no_finish_reason", "Google stream ended without a finish reason"});
        }
        if (out.stopReason == StopReason::Error || out.stopReason == StopReason::Aborted) {
            return std::unexpected(Error{"model_error", out.rawStopReason ? "Provider stopped with: " + *out.rawStopReason
                                                                          : "An unknown error occurred"});
        }
        m_emitter.done(out.stopReason);
        return {};
    }

private:
    void handleChunk(const Json& chunk) {
        AssistantMessage& out = m_emitter.message();
        if (!out.responseId && !stringField(chunk, "responseId").empty()) {
            out.responseId = stringField(chunk, "responseId");
        }
        const Json* candidate = nullptr;
        if (chunk.contains("candidates") && chunk["candidates"].is_array() && !chunk["candidates"].empty()) {
            candidate = &chunk["candidates"][0];
        }
        if (candidate != nullptr && candidate->contains("content") && (*candidate)["content"].contains("parts") &&
            (*candidate)["content"]["parts"].is_array()) {
            for (const auto& part : (*candidate)["content"]["parts"]) {
                handlePart(part);
            }
        }
        if (candidate != nullptr && !stringField(*candidate, "finishReason").empty()) {
            applyFinishReason(stringField(*candidate, "finishReason"));
        }
        if (chunk.contains("usageMetadata") && chunk["usageMetadata"].is_object()) {
            readUsage(chunk["usageMetadata"]);
        }
    }

    void handlePart(const Json& part) {
        if (part.contains("text") && part["text"].is_string()) {
            handleText(part);
        }
        if (part.contains("functionCall") && part["functionCall"].is_object()) {
            handleFunctionCall(part);
        }
    }

    void handleText(const Json& part) {
        const bool thinking = part.contains("thought") && part["thought"] == true;
        if (!m_open || m_openIsThinking != thinking) {
            closeBlock();
            m_open = thinking ? m_emitter.thinkingStart() : m_emitter.textStart();
            m_openIsThinking = thinking;
        }
        const std::string text = part["text"].get<std::string>();
        const std::string signature = stringField(part, "thoughtSignature");
        auto& content = m_emitter.message().content[static_cast<std::size_t>(*m_open)];
        if (thinking) {
            auto& block = std::get<ThinkingContent>(content);
            if (!signature.empty()) {
                block.thinkingSignature = signature;
            }
            m_emitter.thinkingDelta(*m_open, text);
        } else {
            auto& block = std::get<TextContent>(content);
            if (!signature.empty()) {
                block.textSignature = signature;
            }
            m_emitter.textDelta(*m_open, text);
        }
    }

    void handleFunctionCall(const Json& part) {
        closeBlock();
        const Json& call = part["functionCall"];
        const int index = m_emitter.toolCallStart(toolCallId(call), stringField(call, "name"));
        const Json args = call.contains("args") && call["args"].is_object() ? call["args"] : Json::object();
        m_emitter.toolCallDelta(index, args.dump(-1, ' ', false, Json::error_handler_t::replace));
        m_emitter.toolCallEnd(index);
        const std::string signature = stringField(part, "thoughtSignature");
        if (!signature.empty()) {
            std::get<ToolCall>(m_emitter.message().content[static_cast<std::size_t>(index)]).thoughtSignature = signature;
        }
    }

    void closeBlock() {
        if (!m_open) {
            return;
        }
        if (m_openIsThinking) {
            m_emitter.thinkingEnd(*m_open);
        } else {
            m_emitter.textEnd(*m_open);
        }
        m_open.reset();
    }

    void readUsage(const Json& usage) {
        const std::int64_t cached = intField(usage, "cachedContentTokenCount");
        const std::int64_t thoughts = intField(usage, "thoughtsTokenCount");
        Usage out;
        out.input = intField(usage, "promptTokenCount") - cached;
        out.output = intField(usage, "candidatesTokenCount") + thoughts;
        out.cacheRead = cached;
        out.reasoning = thoughts;
        out.totalTokens = intField(usage, "totalTokenCount");
        m_cost.calculate(m_model, out);
        m_emitter.message().usage = out;
    }

    void applyFinishReason(const std::string& reason) {
        AssistantMessage& out = m_emitter.message();
        out.rawStopReason = reason;
        out.stopReason = reason == "STOP" ? StopReason::Stop : reason == "MAX_TOKENS" ? StopReason::Length : StopReason::Error;
        const bool hasTool = std::any_of(out.content.begin(), out.content.end(), [](const auto& block) {
            return std::holds_alternative<ToolCall>(block);
        });
        if (hasTool && out.stopReason == StopReason::Stop) {
            out.stopReason = StopReason::ToolUse;
        }
    }

    std::string toolCallId(const Json& call) {
        const std::string provided = stringField(call, "id");
        bool duplicate = false;
        for (const auto& block : m_emitter.message().content) {
            const auto* existing = std::get_if<ToolCall>(&block);
            duplicate = duplicate || (existing != nullptr && existing->id == provided);
        }
        if (!provided.empty() && !duplicate) {
            return provided;
        }
        return stringField(call, "name") + "_" + std::to_string(m_clock.nowMs()) + "_" + std::to_string(++m_counter);
    }

    std::int64_t intField(const Json& object, const std::string& key) const {
        if (object.is_object() && object.contains(key) && object[key].is_number()) {
            return object[key].get<std::int64_t>();
        }
        return 0;
    }

    std::string stringField(const Json& object, const std::string& key) const {
        if (object.is_object() && object.contains(key) && object[key].is_string()) {
            return object[key].get<std::string>();
        }
        return "";
    }

    AssistantStreamEmitter& m_emitter;
    Model m_model;
    const IClock& m_clock;
    CostCalculator m_cost;
    /** The open text or thinking block, as an index into the message content. */
    std::optional<int> m_open;
    bool m_openIsThinking = false;
    std::int64_t m_counter = 0;
};
