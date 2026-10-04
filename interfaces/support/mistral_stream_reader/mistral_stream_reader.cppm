module;

#include <cstdint>

export module pi.support.mistral_stream_reader;

import std;
export import pi.support.assistant_stream_emitter;
export import pi.support.cost_calculator;
export import pi.support.short_hash;
export import pi.types.json;
export import pi.types.model;
export import pi.types.result;
export import pi.types.sse_event;

/**
 * Turns Mistral chat completion chunks into AssistantStreamEmitter calls: text and thinking
 * chunks, tool calls keyed by their index, usage with cost and finish_reason. The emitter must
 * already have emitted start(). Port of consumeChatStream in api/mistral-conversations.ts.
 */
export class MistralStreamReader {
public:
    MistralStreamReader(AssistantStreamEmitter& emitter, const Model& model)
        : m_emitter(emitter),
          m_model(model) {}

    Result<void> handle(const SseEvent& sse) {
        if (sse.data.empty() || sse.data == "[DONE]") {
            return {};
        }
        const Json chunk = Json::parse(sse.data, nullptr, false);
        if (chunk.is_discarded()) {
            return std::unexpected(Error{"parse_error", "Could not parse Mistral chunk: " + sse.data});
        }
        if (!chunk.is_object() || !chunk.contains("choices") || !chunk["choices"].is_array()) {
            return std::unexpected(Error{"invalid_event", "Invalid Mistral streaming event"});
        }
        handleChunk(chunk);
        return {};
    }

    /** Ends the open block and every tool call, then emits Done or returns why the stream is unusable. */
    Result<void> finish() {
        closeBlock();
        for (const int index : m_toolOrder) {
            m_emitter.toolCallEnd(index);
        }
        AssistantMessage& out = m_emitter.message();
        if (out.stopReason == StopReason::Pending) {
            return std::unexpected(Error{"no_finish_reason", "Mistral stream ended without a finish reason"});
        }
        if (out.stopReason == StopReason::Error || out.stopReason == StopReason::Aborted) {
            return std::unexpected(Error{"model_error", out.errorMessage.value_or("An unknown error occurred")});
        }
        m_emitter.done(out.stopReason);
        return {};
    }

private:
    void handleChunk(const Json& chunk) {
        AssistantMessage& out = m_emitter.message();
        if (!out.responseId && chunk.contains("id") && chunk["id"].is_string() && !chunk["id"].get<std::string>().empty()) {
            out.responseId = chunk["id"].get<std::string>();
        }
        if (chunk.contains("usage") && chunk["usage"].is_object()) {
            readUsage(chunk["usage"]);
        }
        if (chunk["choices"].empty()) {
            return;
        }
        const Json& choice = chunk["choices"][0];
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string() &&
            !choice["finish_reason"].get<std::string>().empty()) {
            applyFinishReason(choice["finish_reason"].get<std::string>());
        }
        if (!choice.contains("delta") || !choice["delta"].is_object()) {
            return;
        }
        const Json& delta = choice["delta"];
        if (delta.contains("content")) {
            handleContent(delta["content"]);
        }
        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            for (const auto& call : delta["tool_calls"]) {
                handleToolCall(call);
            }
        }
    }

    void handleContent(const Json& content) {
        if (content.is_string()) {
            appendBlock(false, content.get<std::string>());
        } else if (content.is_array()) {
            for (const auto& item : content) {
                handleContentItem(item);
            }
        }
    }

    void handleContentItem(const Json& item) {
        if (item.is_string()) {
            appendBlock(false, item.get<std::string>());
            return;
        }
        const std::string type = item.is_object() && item.contains("type") && item["type"].is_string()
                                     ? item["type"].get<std::string>()
                                     : "";
        if (type == "thinking") {
            std::string text;
            if (item.contains("thinking") && item["thinking"].is_array()) {
                for (const auto& part : item["thinking"]) {
                    if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                        text += part["text"].get<std::string>();
                    }
                }
            }
            appendBlock(true, text);
        } else if (type == "text" && item.contains("text") && item["text"].is_string()) {
            appendBlock(false, item["text"].get<std::string>());
        }
    }

    void appendBlock(bool thinking, const std::string& delta) {
        // Empty deltas must not open blocks: some models send them around thinking and tool calls.
        if (delta.empty()) {
            return;
        }
        if (!m_open || m_openIsThinking != thinking) {
            closeBlock();
            m_open = thinking ? m_emitter.thinkingStart() : m_emitter.textStart();
            m_openIsThinking = thinking;
        }
        if (thinking) {
            m_emitter.thinkingDelta(*m_open, delta);
        } else {
            m_emitter.textDelta(*m_open, delta);
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

    void handleToolCall(const Json& call) {
        closeBlock();
        const std::string id = callId(call);
        const std::string key = call.contains("index") && call["index"].is_number()
                                    ? "i:" + std::to_string(call["index"].get<std::int64_t>())
                                    : "id:" + id;
        const Json function = call.contains("function") && call["function"].is_object() ? call["function"] : Json::object();
        auto found = m_toolByKey.find(key);
        if (found == m_toolByKey.end()) {
            const std::string name = function.contains("name") && function["name"].is_string()
                                         ? function["name"].get<std::string>()
                                         : "";
            const int index = m_emitter.toolCallStart(id, name);
            found = m_toolByKey.emplace(key, index).first;
            m_toolOrder.push_back(index);
        }
        std::string delta;
        if (function.contains("arguments")) {
            delta = function["arguments"].is_string()
                        ? function["arguments"].get<std::string>()
                        : function["arguments"].dump(-1, ' ', false, Json::error_handler_t::replace);
        }
        m_emitter.toolCallDelta(found->second, delta);
    }

    void readUsage(const Json& usage) {
        const std::int64_t promptTokens = numberIn(usage, "prompt_tokens");
        const std::int64_t cached = cachedTokens(usage, promptTokens);
        Usage out;
        out.input = std::max<std::int64_t>(0, promptTokens - cached);
        out.output = numberIn(usage, "completion_tokens");
        out.cacheRead = cached;
        out.totalTokens = numberIn(usage, "total_tokens");
        if (out.totalTokens == 0) {
            out.totalTokens = out.input + out.output + out.cacheRead;
        }
        m_cost.calculate(m_model, out);
        m_emitter.message().usage = out;
    }

    void applyFinishReason(const std::string& reason) {
        AssistantMessage& out = m_emitter.message();
        out.rawStopReason = reason;
        if (reason == "stop") {
            out.stopReason = StopReason::Stop;
        } else if (reason == "length" || reason == "model_length") {
            out.stopReason = StopReason::Length;
        } else if (reason == "tool_calls") {
            out.stopReason = StopReason::ToolUse;
        } else {
            out.stopReason = StopReason::Error;
            out.errorMessage = "Provider stopped with: " + reason;
        }
    }

    std::int64_t cachedTokens(const Json& usage, std::int64_t promptTokens) const {
        std::int64_t cached = 0;
        for (const std::string detail : {"promptTokensDetails", "prompt_tokens_details", "promptTokenDetails",
                                          "prompt_token_details"}) {
            if (usage.contains(detail) && usage[detail].is_object()) {
                for (const std::string field : {"cachedTokens", "cached_tokens"}) {
                    if (usage[detail].contains(field) && usage[detail][field].is_number()) {
                        cached = usage[detail][field].get<std::int64_t>();
                        return std::min(promptTokens, std::max<std::int64_t>(0, cached));
                    }
                }
            }
        }
        for (const std::string field : {"numCachedTokens", "num_cached_tokens"}) {
            if (usage.contains(field) && usage[field].is_number()) {
                cached = usage[field].get<std::int64_t>();
                break;
            }
        }
        return std::min(promptTokens, std::max<std::int64_t>(0, cached));
    }

    std::int64_t numberIn(const Json& object, const std::string& key) const {
        if (object.is_object() && object.contains(key) && object[key].is_number()) {
            return object[key].get<std::int64_t>();
        }
        return 0;
    }

    std::string callId(const Json& call) const {
        if (call.contains("id") && call["id"].is_string() && call["id"].get<std::string>() != "null" &&
            !call["id"].get<std::string>().empty()) {
            return call["id"].get<std::string>();
        }
        std::string hashed;
        for (const char c : m_hash.of("toolcall:" + std::to_string(numberIn(call, "index")))) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
                hashed.push_back(c);
            }
        }
        return hashed.substr(0, 9);
    }

    AssistantStreamEmitter& m_emitter;
    Model m_model;
    CostCalculator m_cost;
    ShortHash m_hash;
    std::optional<int> m_open;
    bool m_openIsThinking = false;
    std::map<std::string, int> m_toolByKey;
    std::vector<int> m_toolOrder;
};
