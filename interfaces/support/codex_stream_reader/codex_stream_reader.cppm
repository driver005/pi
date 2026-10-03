export module pi.support.codex_stream_reader;

import std;
export import pi.support.assistant_stream_emitter;
export import pi.support.responses_stream_reader;
export import pi.types.json;
export import pi.types.model;
export import pi.types.result;
export import pi.types.sse_event;

/**
 * Reads the Codex backend's Responses event stream: Codex error and failure events get their own
 * messages, response.done/completed/incomplete become one completed response (with the end_turn
 * flag recorded), and everything else goes to the shared ResponsesStreamReader. Events after the
 * terminal one are ignored. Port of mapCodexEvents in api/openai-codex-responses.ts.
 */
export class CodexStreamReader {
public:
    CodexStreamReader(AssistantStreamEmitter& emitter, const Model& model);

    Result<void> handle(const SseEvent& event);
    Result<void> finish();

private:
    Result<void> terminal(const Json& event);
    std::string stringIn(const Json& object, const std::string& key) const;

    AssistantStreamEmitter& m_emitter;
    ResponsesStreamReader m_responses;
    bool m_completed = false;
};

CodexStreamReader::CodexStreamReader(AssistantStreamEmitter& emitter, const Model& model)
    : m_emitter(emitter), m_responses(emitter, model) {}

std::string CodexStreamReader::stringIn(const Json& object, const std::string& key) const {
    return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : "";
}

Result<void> CodexStreamReader::terminal(const Json& event) {
    m_completed = true;
    Json forwarded = event;
    forwarded["type"] = "response.completed";
    if (forwarded.contains("response") && forwarded["response"].is_object()) {
        Json& response = forwarded["response"];
        if (response.contains("end_turn") && response["end_turn"].is_boolean()) {
            m_emitter.message().endTurn = response["end_turn"].get<bool>();
        }
        const std::string status = stringIn(response, "status");
        const bool known = status == "completed" || status == "incomplete" || status == "failed" ||
                           status == "cancelled" || status == "queued" || status == "in_progress";
        if (!known) {
            response.erase("status");
        }
    }
    SseEvent sse;
    sse.data = forwarded.dump(-1, ' ', false, Json::error_handler_t::replace);
    return m_responses.handle(sse);
}

Result<void> CodexStreamReader::handle(const SseEvent& sse) {
    if (sse.data.empty() || sse.data == "[DONE]" || m_completed) {
        return {};
    }
    const Json event = Json::parse(sse.data, nullptr, false);
    if (event.is_discarded()) {
        return std::unexpected(Error{"protocol_error", "Invalid Codex SSE JSON: " + sse.data});
    }
    const std::string type = stringIn(event, "type");
    if (type == "error") {
        const Json nested = event.contains("error") && event["error"].is_object() ? event["error"] : Json::object();
        const std::string code = !stringIn(event, "code").empty() ? stringIn(event, "code") : stringIn(nested, "code");
        const std::string message =
            !stringIn(event, "message").empty() ? stringIn(event, "message") : stringIn(nested, "message");
        return std::unexpected(Error{code.empty() ? "codex_error" : code,
                                     "Codex error: " + (!message.empty() ? message : !code.empty() ? code
                                                                                                    : event.dump())});
    }
    if (type == "response.failed") {
        const Json response = event.contains("response") && event["response"].is_object() ? event["response"] : Json::object();
        const Json error = response.contains("error") && response["error"].is_object() ? response["error"] : Json::object();
        const std::string message = stringIn(error, "message");
        return std::unexpected(Error{stringIn(error, "code").empty() ? "codex_error" : stringIn(error, "code"),
                                     message.empty() ? "Codex response failed" : message});
    }
    if (type == "response.done" || type == "response.completed" || type == "response.incomplete") {
        return terminal(event);
    }
    return m_responses.handle(sse);
}

Result<void> CodexStreamReader::finish() {
    return m_responses.finish();
}
