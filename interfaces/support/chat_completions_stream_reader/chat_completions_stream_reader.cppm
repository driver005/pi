module;

#include <cstdint>

export module pi.support.chat_completions_stream_reader;

import std;
export import pi.support.assistant_stream_emitter;
export import pi.support.cost_calculator;
export import pi.types.chat_completions_compat;
export import pi.types.model;
export import pi.types.result;
export import pi.types.sse_event;

/**
 * Turns OpenAI-compatible chat completion chunks into AssistantStreamEmitter calls: text,
 * reasoning (reasoning_content / reasoning / reasoning_text / reasoning_details), tool calls,
 * usage with cost and finish_reason. The emitter must already have emitted start().
 * Port of the chunk loop, parseChunkUsage and mapStopReason in api/openai-completions.ts.
 */
export class ChatCompletionsStreamReader {
public:
    ChatCompletionsStreamReader(AssistantStreamEmitter& emitter, const Model& model, const ChatCompletionsCompat& compat)
        : m_emitter(emitter),
          m_model(model),
          m_compat(compat) {}

    Result<void> handle(const SseEvent& sse) {
        if (sse.event == "error") {
            return std::unexpected(Error{"stream_error", sse.data});
        }
        if (sse.data == "[DONE]") {
            return {};
        }
        const Json chunk = Json::parse(sse.data, nullptr, false);
        if (chunk.is_discarded()) {
            return std::unexpected(Error{"parse_error", "Could not parse chat completion chunk: " + sse.data});
        }
        if (!chunk.is_object()) {
            return {};
        }
        if (chunk.contains("error") && !chunk["error"].is_null()) {
            const Json& error = chunk["error"];
            std::string message = error.is_object() && error.contains("message") && error["message"].is_string()
                                      ? error["message"].get<std::string>()
                                      : error.dump(-1, ' ', false, Json::error_handler_t::replace);
            return std::unexpected(Error{"provider_error", message});
        }
        handleChunk(chunk);
        return {};
    }

    /** Ends every open block and emits Done, or returns why the stream is unusable. */
    Result<void> finish() {
        AssistantMessage& out = m_emitter.message();
        const std::size_t count = out.content.size();
        for (std::size_t i = 0; i < count; ++i) {
            const int index = static_cast<int>(i);
            if (std::holds_alternative<TextContent>(out.content[i])) {
                m_emitter.textEnd(index);
            } else if (auto* thinking = std::get_if<ThinkingContent>(&out.content[i])) {
                if (m_details.is_array()) {
                    thinking->thinkingSignature = m_details.dump(-1, ' ', false, Json::error_handler_t::replace);
                }
                m_emitter.thinkingEnd(index);
            } else {
                m_emitter.toolCallEnd(index);
            }
        }
        if (!m_hasFinishReason && !m_compat.supportsFinishReason) {
            const bool hasTool = std::any_of(out.content.begin(), out.content.end(), [](const auto& block) {
                return std::holds_alternative<ToolCall>(block);
            });
            out.stopReason = hasTool ? StopReason::ToolUse : StopReason::Stop;
        }
        if (out.stopReason == StopReason::Error) {
            return std::unexpected(
                Error{"model_error", out.errorMessage.value_or("Provider returned an error stop reason")});
        }
        if ((m_compat.supportsFinishReason && !m_hasFinishReason) || out.stopReason == StopReason::Pending) {
            return std::unexpected(Error{"no_finish_reason", "Stream ended without finish_reason"});
        }
        m_emitter.done(out.stopReason);
        return {};
    }

private:
    void handleChunk(const Json& chunk) {
        AssistantMessage& out = m_emitter.message();
        if (!out.responseId && chunk.contains("id") && chunk["id"].is_string() &&
            !chunk["id"].get<std::string>().empty()) {
            out.responseId = chunk["id"].get<std::string>();
        }
        if (chunk.contains("model") && chunk["model"].is_string()) {
            const std::string model = chunk["model"].get<std::string>();
            if (!model.empty() && model != m_model.id && !out.responseModel) {
                out.responseModel = model;
            }
        }
        const bool hasUsage = chunk.contains("usage") && chunk["usage"].is_object();
        if (hasUsage) {
            readUsage(chunk["usage"]);
        }
        if (!chunk.contains("choices") || !chunk["choices"].is_array() || chunk["choices"].empty()) {
            return;
        }
        const Json& choice = chunk["choices"][0];
        if (!hasUsage && choice.contains("usage") && choice["usage"].is_object()) {
            readUsage(choice["usage"]);
        }
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
            applyFinishReason(choice["finish_reason"].get<std::string>());
        }
        if (choice.contains("delta") && choice["delta"].is_object()) {
            handleDelta(choice["delta"]);
        }
    }

    void handleDelta(const Json& delta) {
        if (delta.contains("content") && delta["content"].is_string() &&
            !delta["content"].get<std::string>().empty()) {
            handleText(delta["content"].get<std::string>());
        }
        handleReasoning(delta);
        if (delta.contains("tool_calls")) {
            handleToolCalls(delta["tool_calls"]);
        }
        if (delta.contains("reasoning_details")) {
            handleReasoningDetails(delta["reasoning_details"]);
        }
    }

    void handleText(const std::string& text) {
        m_emitter.textDelta(ensureText(), text);
    }

    void handleReasoning(const Json& delta) {
        for (const std::string field : {"reasoning_content", "reasoning", "reasoning_text"}) {
            if (delta.contains(field) && delta[field].is_string() && !delta[field].get<std::string>().empty()) {
                const std::string signature = m_model.provider == "opencode-go" && field == "reasoning"
                                                  ? "reasoning_content"
                                                  : field;
                m_emitter.thinkingDelta(ensureThinking(signature), delta[field].get<std::string>());
                return;
            }
        }
    }

    void handleToolCalls(const Json& calls) {
        if (!calls.is_array()) {
            return;
        }
        for (const auto& call : calls) {
            if (!call.is_object()) {
                continue;
            }
            const int index = ensureToolCall(call);
            std::string fragment;
            if (call.contains("function") && call["function"].is_object() &&
                call["function"].contains("arguments") && call["function"]["arguments"].is_string()) {
                fragment = call["function"]["arguments"].get<std::string>();
            }
            m_emitter.toolCallDelta(index, fragment);
        }
    }

    void handleReasoningDetails(const Json& details) {
        if (!details.is_array()) {
            return;
        }
        for (const auto& detail : details) {
            if (!validReasoningDetail(detail)) {
                continue;
            }
            ensureThinking("");
            appendReasoningDetail(detail);
        }
    }

    void appendReasoningDetail(const Json& detail) {
        if (!m_details.is_array()) {
            m_details = Json::array();
        }
        const std::string type = detail["type"].get<std::string>();
        if (!m_details.empty()) {
            Json& last = m_details.back();
            const std::string lastType = last.value("type", "");
            const bool mergeable = (type == "reasoning.text" || type == "reasoning.summary") && lastType == type;
            if (mergeable) {
                const std::string key = type == "reasoning.text" ? "text" : "summary";
                last[key] = last[key].get<std::string>() + detail[key].get<std::string>();
                if (type == "reasoning.text" && detail.contains("signature") && detail["signature"].is_string() &&
                    (!last.contains("signature") || last["signature"].is_null() ||
                     last["signature"].get<std::string>().empty())) {
                    last["signature"] = detail["signature"];
                }
                for (const std::string field : {"id", "format", "index"}) {
                    if ((!last.contains(field) || last[field].is_null()) && detail.contains(field)) {
                        last[field] = detail[field];
                    }
                }
                return;
            }
        }
        m_details.push_back(detail);
    }

    int ensureText() {
        if (!m_textIndex) {
            m_textIndex = m_emitter.textStart();
        }
        return *m_textIndex;
    }

    int ensureThinking(const std::string& signature) {
        if (!m_thinkingIndex) {
            m_thinkingIndex = m_emitter.thinkingStart();
            std::get<ThinkingContent>(m_emitter.message().content[static_cast<std::size_t>(*m_thinkingIndex)])
                .thinkingSignature = signature;
        }
        return *m_thinkingIndex;
    }

    int ensureToolCall(const Json& call) {
        std::optional<int> streamIndex;
        if (call.contains("index") && call["index"].is_number_integer()) {
            streamIndex = call["index"].get<int>();
        }
        const std::string id = call.contains("id") && call["id"].is_string() ? call["id"].get<std::string>() : "";
        const Json function = call.contains("function") && call["function"].is_object() ? call["function"] : Json::object();
        const std::string name = function.contains("name") && function["name"].is_string()
                                     ? function["name"].get<std::string>()
                                     : "";
        std::optional<int> found;
        if (streamIndex && m_toolByStreamIndex.contains(*streamIndex)) {
            found = m_toolByStreamIndex[*streamIndex];
        }
        if (!found && !id.empty() && m_toolById.contains(id)) {
            found = m_toolById[id];
        }
        if (!found) {
            found = m_emitter.toolCallStart(id, name);
        }
        auto& block = std::get<ToolCall>(m_emitter.message().content[static_cast<std::size_t>(*found)]);
        if (streamIndex) {
            m_toolByStreamIndex[*streamIndex] = *found;
        }
        if (!id.empty()) {
            m_toolById[id] = *found;
            if (block.id.empty()) {
                block.id = id;
            }
        }
        if (block.name.empty() && !name.empty()) {
            block.name = name;
        }
        return *found;
    }

    void readUsage(const Json& usage) {
        const std::int64_t promptTokens = intField(usage, "prompt_tokens");
        const Json details = usage.contains("prompt_tokens_details") ? usage["prompt_tokens_details"] : Json();
        std::int64_t cacheRead = 0;
        if (details.is_object() && details.contains("cached_tokens") && details["cached_tokens"].is_number()) {
            cacheRead = details["cached_tokens"].get<std::int64_t>();
        } else if (usage.contains("prompt_cache_hit_tokens") && usage["prompt_cache_hit_tokens"].is_number()) {
            cacheRead = usage["prompt_cache_hit_tokens"].get<std::int64_t>();
        } else {
            cacheRead = intField(usage, "cached_tokens");
        }
        const std::int64_t cacheWrite = intField(details, "cache_write_tokens");
        Usage out;
        out.input = std::max<std::int64_t>(0, promptTokens - cacheRead - cacheWrite);
        out.output = intField(usage, "completion_tokens");
        out.cacheRead = cacheRead;
        out.cacheWrite = cacheWrite;
        out.reasoning = intField(usage.contains("completion_tokens_details") ? usage["completion_tokens_details"] : Json(),
                                 "reasoning_tokens");
        out.totalTokens = out.input + out.output + out.cacheRead + out.cacheWrite;
        m_cost.calculate(m_model, out);
        m_emitter.message().usage = out;
    }

    void applyFinishReason(const std::string& reason) {
        AssistantMessage& out = m_emitter.message();
        out.rawStopReason = reason;
        m_hasFinishReason = true;
        if (reason == "stop" || reason == "end") {
            out.stopReason = StopReason::Stop;
        } else if (reason == "length") {
            out.stopReason = StopReason::Length;
        } else if (reason == "function_call" || reason == "tool_calls") {
            out.stopReason = StopReason::ToolUse;
        } else {
            out.stopReason = StopReason::Error;
            out.errorMessage = "Provider finish_reason: " + reason;
        }
    }

    std::int64_t intField(const Json& object, const std::string& key) const {
        if (object.is_object() && object.contains(key) && object[key].is_number()) {
            return object[key].get<std::int64_t>();
        }
        return 0;
    }

    bool validReasoningDetail(const Json& detail) const {
        if (!detail.is_object() || !detail.contains("type") || !detail["type"].is_string()) {
            return false;
        }
        const std::string type = detail["type"].get<std::string>();
        if (type == "reasoning.summary") {
            return detail.contains("summary") && detail["summary"].is_string();
        }
        if (type == "reasoning.encrypted") {
            return detail.contains("data") && detail["data"].is_string();
        }
        return type == "reasoning.text" && detail.contains("text") && detail["text"].is_string();
    }

    AssistantStreamEmitter& m_emitter;
    Model m_model;
    ChatCompletionsCompat m_compat;
    CostCalculator m_cost;
    std::optional<int> m_textIndex;
    std::optional<int> m_thinkingIndex;
    std::map<int, int> m_toolByStreamIndex;
    std::map<std::string, int> m_toolById;
    Json m_details;
    bool m_hasFinishReason = false;
};
