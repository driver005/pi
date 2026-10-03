module;

#include <cstdint>

export module pi.support.responses_stream_reader;

import std;
export import pi.support.assistant_stream_emitter;
export import pi.support.cost_calculator;
export import pi.types.json;
export import pi.types.model;
export import pi.types.result;
export import pi.types.sse_event;

/**
 * Turns OpenAI Responses stream events into AssistantStreamEmitter calls: reasoning summaries,
 * text and refusals, function calls, usage with cost and the terminal response status. Output
 * items are tracked by their output_index. The emitter must already have emitted start().
 * Port of processResponsesStream in api/openai-responses-shared.ts (custom tool calls and the
 * message-phase stop shortcut are not ported).
 */
export class ResponsesStreamReader {
public:
    ResponsesStreamReader(AssistantStreamEmitter& emitter, const Model& model);

    Result<void> handle(const SseEvent& event);

    /** Checks the stream ended properly and emits Done, or returns why it is unusable. */
    Result<void> finish();

private:
    Result<void> dispatch(const std::string& type, const Json& event);
    void itemAdded(int outputIndex, const Json& item);
    void itemDone(int outputIndex, const Json& item);
    void reasoningDone(int outputIndex, const Json& item);
    void messageDone(int outputIndex, const Json& item);
    void functionCallDone(int outputIndex, const Json& item);
    void appendThinking(const Json& event, const std::string& delta);
    void appendText(const Json& event);
    void toolArgumentsDelta(const Json& event);
    void toolArgumentsDone(const Json& event);
    void completed(const Json& response);
    void readUsage(const Json& response);
    void backfillSignatures(const Json& output);
    Result<void> failed(const Json& response);
    std::optional<int> slot(const std::map<int, int>& slots, const Json& event) const;
    std::string joinText(const Json& parts, const std::string& field, const std::string& separator) const;
    std::string textSignature(const std::string& id, const std::string& phase) const;
    std::int64_t intField(const Json& object, const std::string& key) const;
    std::string stringField(const Json& object, const std::string& key) const;

    AssistantStreamEmitter& m_emitter;
    Model m_model;
    CostCalculator m_cost;
    std::map<int, int> m_thinking;
    std::map<int, int> m_text;
    std::map<int, int> m_tools;
    std::map<int, std::string> m_toolJson;
    std::map<std::string, int> m_reasoningById;
    bool m_terminal = false;
};

ResponsesStreamReader::ResponsesStreamReader(AssistantStreamEmitter& emitter, const Model& model)
    : m_emitter(emitter), m_model(model) {}

std::int64_t ResponsesStreamReader::intField(const Json& object, const std::string& key) const {
    if (object.is_object() && object.contains(key) && object[key].is_number()) {
        return object[key].get<std::int64_t>();
    }
    return 0;
}

std::string ResponsesStreamReader::stringField(const Json& object, const std::string& key) const {
    if (object.is_object() && object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return "";
}

std::optional<int> ResponsesStreamReader::slot(const std::map<int, int>& slots, const Json& event) const {
    const auto found = slots.find(static_cast<int>(intField(event, "output_index")));
    return found == slots.end() ? std::nullopt : std::optional<int>(found->second);
}

std::string ResponsesStreamReader::joinText(const Json& parts, const std::string& field,
                                            const std::string& separator) const {
    std::string out;
    bool first = true;
    if (parts.is_array()) {
        for (const auto& part : parts) {
            out += (first ? "" : separator) + stringField(part, field);
            first = false;
        }
    }
    return out;
}

std::string ResponsesStreamReader::textSignature(const std::string& id, const std::string& phase) const {
    Json payload = Json::object();
    payload["v"] = 1;
    payload["id"] = id;
    if (!phase.empty()) {
        payload["phase"] = phase;
    }
    return payload.dump();
}

Result<void> ResponsesStreamReader::handle(const SseEvent& sse) {
    if (sse.data.empty() || sse.data == "[DONE]") {
        return {};
    }
    const Json event = Json::parse(sse.data, nullptr, false);
    if (event.is_discarded()) {
        return std::unexpected(Error{"parse_error", "Could not parse Responses event: " + sse.data});
    }
    if (!event.is_object()) {
        return {};
    }
    const std::string type = event.contains("type") ? stringField(event, "type") : sse.event;
    return dispatch(type, event);
}

Result<void> ResponsesStreamReader::dispatch(const std::string& type, const Json& event) {
    const int outputIndex = static_cast<int>(intField(event, "output_index"));
    if (type == "response.created") {
        if (event.contains("response")) {
            m_emitter.message().responseId = stringField(event["response"], "id");
        }
    } else if (type == "response.output_item.added") {
        itemAdded(outputIndex, event.contains("item") ? event["item"] : Json());
    } else if (type == "response.reasoning_summary_text.delta" || type == "response.reasoning_text.delta") {
        appendThinking(event, stringField(event, "delta"));
    } else if (type == "response.reasoning_summary_part.done") {
        appendThinking(event, "\n\n");
    } else if (type == "response.output_text.delta" || type == "response.refusal.delta") {
        appendText(event);
    } else if (type == "response.function_call_arguments.delta") {
        toolArgumentsDelta(event);
    } else if (type == "response.function_call_arguments.done") {
        toolArgumentsDone(event);
    } else if (type == "response.output_item.done") {
        itemDone(outputIndex, event.contains("item") ? event["item"] : Json());
    } else if (type == "response.completed" || type == "response.incomplete") {
        completed(event.contains("response") ? event["response"] : Json());
    } else if (type == "error") {
        return std::unexpected(Error{"provider_error", "Error Code " + stringField(event, "code") + ": " +
                                                           stringField(event, "message")});
    } else if (type == "response.failed") {
        return failed(event.contains("response") ? event["response"] : Json());
    }
    return {};
}

void ResponsesStreamReader::itemAdded(int outputIndex, const Json& item) {
    const std::string type = stringField(item, "type");
    if (type == "reasoning") {
        m_thinking[outputIndex] = m_emitter.thinkingStart();
    } else if (type == "message") {
        m_text[outputIndex] = m_emitter.textStart();
    } else if (type == "function_call") {
        const int index = m_emitter.toolCallStart(stringField(item, "call_id") + "|" + stringField(item, "id"),
                                                  stringField(item, "name"));
        auto& call = std::get<ToolCall>(m_emitter.message().content[static_cast<std::size_t>(index)]);
        if (item.contains("namespace") && item["namespace"].is_string()) {
            call.toolNamespace = item["namespace"].get<std::string>();
        }
        m_tools[outputIndex] = index;
        const std::string initial = stringField(item, "arguments");
        m_toolJson[outputIndex] = initial;
        if (!initial.empty()) {
            m_emitter.toolCallDelta(index, initial);
        }
    }
}

void ResponsesStreamReader::appendThinking(const Json& event, const std::string& delta) {
    if (const auto index = slot(m_thinking, event)) {
        m_emitter.thinkingDelta(*index, delta);
    }
}

void ResponsesStreamReader::appendText(const Json& event) {
    if (const auto index = slot(m_text, event)) {
        m_emitter.textDelta(*index, stringField(event, "delta"));
    }
}

void ResponsesStreamReader::toolArgumentsDelta(const Json& event) {
    const auto index = slot(m_tools, event);
    if (!index) {
        return;
    }
    const std::string delta = stringField(event, "delta");
    m_toolJson[static_cast<int>(intField(event, "output_index"))] += delta;
    m_emitter.toolCallDelta(*index, delta);
}

void ResponsesStreamReader::toolArgumentsDone(const Json& event) {
    const auto index = slot(m_tools, event);
    if (!index) {
        return;
    }
    std::string& seen = m_toolJson[static_cast<int>(intField(event, "output_index"))];
    const std::string full = stringField(event, "arguments");
    if (full.starts_with(seen) && full.size() > seen.size()) {
        m_emitter.toolCallDelta(*index, full.substr(seen.size()));
    }
    seen = full;
}

void ResponsesStreamReader::itemDone(int outputIndex, const Json& item) {
    const std::string type = stringField(item, "type");
    if (type == "reasoning") {
        reasoningDone(outputIndex, item);
    } else if (type == "message") {
        messageDone(outputIndex, item);
    } else if (type == "function_call") {
        functionCallDone(outputIndex, item);
    }
}

void ResponsesStreamReader::reasoningDone(int outputIndex, const Json& item) {
    if (!m_thinking.contains(outputIndex)) {
        m_thinking[outputIndex] = m_emitter.thinkingStart();
    }
    const int index = m_thinking[outputIndex];
    auto& block = std::get<ThinkingContent>(m_emitter.message().content[static_cast<std::size_t>(index)]);
    const std::string summary = joinText(item.contains("summary") ? item["summary"] : Json(), "text", "\n\n");
    const std::string content = joinText(item.contains("content") ? item["content"] : Json(), "text", "\n\n");
    block.thinking = !summary.empty() ? summary : !content.empty() ? content : block.thinking;
    block.thinkingSignature = item.dump(-1, ' ', false, Json::error_handler_t::replace);
    m_reasoningById[stringField(item, "id")] = index;
    m_emitter.thinkingEnd(index);
    m_thinking.erase(outputIndex);
}

void ResponsesStreamReader::messageDone(int outputIndex, const Json& item) {
    if (!m_text.contains(outputIndex)) {
        m_text[outputIndex] = m_emitter.textStart();
    }
    const int index = m_text[outputIndex];
    auto& block = std::get<TextContent>(m_emitter.message().content[static_cast<std::size_t>(index)]);
    std::string text;
    if (item.contains("content") && item["content"].is_array()) {
        for (const auto& part : item["content"]) {
            text += stringField(part, stringField(part, "type") == "output_text" ? "text" : "refusal");
        }
    }
    block.text = text;
    block.textSignature = textSignature(stringField(item, "id"), stringField(item, "phase"));
    m_emitter.textEnd(index);
    m_text.erase(outputIndex);
}

void ResponsesStreamReader::functionCallDone(int outputIndex, const Json& item) {
    if (!m_tools.contains(outputIndex)) {
        return;
    }
    const int index = m_tools[outputIndex];
    const std::string previous = m_toolJson[outputIndex];
    const std::string full = stringField(item, "arguments");
    const std::string finalJson = !full.empty() ? full : !previous.empty() ? previous : "{}";
    if (finalJson.starts_with(previous) && finalJson.size() > previous.size()) {
        m_emitter.toolCallDelta(index, finalJson.substr(previous.size()));
    }
    m_emitter.toolCallEnd(index);
    auto& call = std::get<ToolCall>(m_emitter.message().content[static_cast<std::size_t>(index)]);
    if (!finalJson.starts_with(previous)) {
        const Json parsed = Json::parse(finalJson, nullptr, false);
        call.arguments = parsed.is_object() ? parsed : Json::object();
    }
    if (item.contains("namespace") && item["namespace"].is_string()) {
        call.toolNamespace = item["namespace"].get<std::string>();
    }
    m_tools.erase(outputIndex);
    m_toolJson.erase(outputIndex);
}

void ResponsesStreamReader::backfillSignatures(const Json& output) {
    if (!output.is_array()) {
        return;
    }
    for (const auto& item : output) {
        const auto found = m_reasoningById.find(stringField(item, "id"));
        if (stringField(item, "type") != "reasoning" || stringField(item, "encrypted_content").empty() ||
            found == m_reasoningById.end()) {
            continue;
        }
        auto& block = std::get<ThinkingContent>(m_emitter.message().content[static_cast<std::size_t>(found->second)]);
        if (!block.thinkingSignature) {
            continue;
        }
        Json stored = Json::parse(*block.thinkingSignature, nullptr, false);
        if (stored.is_object() && stringField(stored, "encrypted_content").empty()) {
            stored["encrypted_content"] = item["encrypted_content"];
            block.thinkingSignature = stored.dump(-1, ' ', false, Json::error_handler_t::replace);
        }
    }
}

void ResponsesStreamReader::readUsage(const Json& response) {
    if (!response.contains("usage") || !response["usage"].is_object()) {
        m_cost.calculate(m_model, m_emitter.message().usage);
        return;
    }
    const Json& usage = response["usage"];
    const Json details = usage.contains("input_tokens_details") ? usage["input_tokens_details"] : Json();
    const std::int64_t cached = intField(details, "cached_tokens");
    const std::int64_t written = intField(details, "cache_write_tokens");
    Usage out;
    // OpenAI counts cached and cache-write tokens inside input_tokens.
    out.input = std::max<std::int64_t>(0, intField(usage, "input_tokens") - cached - written);
    out.output = intField(usage, "output_tokens");
    out.cacheRead = cached;
    out.cacheWrite = written;
    out.reasoning = intField(usage.contains("output_tokens_details") ? usage["output_tokens_details"] : Json(),
                             "reasoning_tokens");
    out.totalTokens = intField(usage, "total_tokens");
    m_cost.calculate(m_model, out);
    m_emitter.message().usage = out;
}

void ResponsesStreamReader::completed(const Json& response) {
    m_terminal = true;
    AssistantMessage& out = m_emitter.message();
    backfillSignatures(response.contains("output") ? response["output"] : Json());
    if (!stringField(response, "id").empty()) {
        out.responseId = stringField(response, "id");
    }
    readUsage(response);
    const std::string status = stringField(response, "status");
    std::string reason;
    if (response.contains("incomplete_details") && response["incomplete_details"].is_object()) {
        reason = stringField(response["incomplete_details"], "reason");
    }
    if (!status.empty()) {
        out.rawStopReason = reason.empty() ? status : status + "." + reason;
    }
    out.errorMessage.reset();
    if (status.empty() || status == "completed" || status == "in_progress" || status == "queued") {
        out.stopReason = StopReason::Stop;
    } else if (status == "incomplete" && reason == "max_output_tokens") {
        out.stopReason = StopReason::Length;
    } else if (status == "incomplete") {
        out.stopReason = StopReason::Error;
        out.errorMessage = reason.empty() ? "Response incomplete without a provider reason"
                                          : "Response incomplete: " + reason;
    } else {
        out.stopReason = StopReason::Error;
    }
    const bool hasTool = std::any_of(out.content.begin(), out.content.end(), [](const auto& block) {
        return std::holds_alternative<ToolCall>(block);
    });
    if (hasTool && out.stopReason == StopReason::Stop) {
        out.stopReason = StopReason::ToolUse;
    }
}

Result<void> ResponsesStreamReader::failed(const Json& response) {
    m_terminal = true;
    m_emitter.message().rawStopReason = stringField(response, "status");
    std::string message = "Unknown error (no error details in response)";
    if (response.contains("error") && response["error"].is_object()) {
        const Json& error = response["error"];
        const std::string code = stringField(error, "code");
        const std::string text = stringField(error, "message");
        message = (code.empty() ? "unknown" : code) + ": " + (text.empty() ? "no message" : text);
    } else if (response.contains("incomplete_details") && response["incomplete_details"].is_object() &&
               !stringField(response["incomplete_details"], "reason").empty()) {
        message = "incomplete: " + stringField(response["incomplete_details"], "reason");
    }
    return std::unexpected(Error{"model_error", message});
}

Result<void> ResponsesStreamReader::finish() {
    if (!m_terminal) {
        return std::unexpected(
            Error{"stream_ended", "OpenAI Responses stream ended before a terminal response event"});
    }
    AssistantMessage& out = m_emitter.message();
    if (out.stopReason == StopReason::ToolUse && !m_tools.empty()) {
        const int index = m_tools.begin()->second;
        const auto& call = std::get<ToolCall>(out.content[static_cast<std::size_t>(index)]);
        return std::unexpected(Error{"unfinished_tool_call",
                                     "OpenAI Responses stream completed with an unfinished tool call: " +
                                         call.name + " (" + call.id + ")"});
    }
    if (out.stopReason == StopReason::Error) {
        return std::unexpected(Error{"model_error", out.errorMessage.value_or("An unknown error occurred")});
    }
    if (out.stopReason == StopReason::Pending) {
        return std::unexpected(Error{"no_stop_reason", "OpenAI Responses stream ended without a stop reason"});
    }
    m_emitter.done(out.stopReason);
    return {};
}
