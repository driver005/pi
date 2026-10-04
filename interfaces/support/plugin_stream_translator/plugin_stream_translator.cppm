export module pi.support.plugin_stream_translator;

import std;
export import pi.provider.i_provider;
import pi.support.assistant_stream_emitter;
import pi.support.cost_calculator;
import pi.types.json;

/**
 * Turns the events a plugin's stream function reports (PiHostApi.stream_emit) into the AssistantMessageStream of the request:
 * it builds the response block by block with an AssistantStreamEmitter, so the plugin writes deltas instead of snapshots.
 * Consecutive text deltas form one text block, consecutive thinking deltas one reasoning block, a tool call is a block of its
 * own. An invalid event or one after the end finishes the stream with an error, so a plugin cannot leave readers waiting;
 * finish() closes a stream whose function returned without a final event.
 */
export class PluginStreamTranslator {
public:
    PluginStreamTranslator(const Model& model, std::shared_ptr<AbortSignal> signal, std::int64_t timestamp)
        : m_model(model),
          m_signal(std::move(signal)),
          m_emitter(initial(model, timestamp)) {}

    std::shared_ptr<AssistantMessageStream> stream() const {
        return m_emitter.stream();
    }

    /** Handles one event; false once the response is over (the plugin should stop). */
    bool emit(const std::string& eventText) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_finished) {
            return false;
        }
        const Json event = Json::parse(eventText, nullptr, false);
        if (!event.is_object() || !event.contains("type") || !event["type"].is_string()) {
            return fail("the plugin sent an event that is not a JSON object with a type");
        }
        if (m_signal && m_signal->aborted()) {
            return abort();
        }
        if (!m_started) {
            m_emitter.start();
            m_started = true;
        }
        const std::string type = event["type"].get<std::string>();
        if (type == "text_delta" || type == "thinking_delta") {
            return delta(type == "text_delta", event);
        }
        if (type == "tool_call") {
            return toolCall(event);
        }
        if (type == "usage") {
            return usage(event);
        }
        if (type == "response") {
            return response(event);
        }
        if (type == "done") {
            return done(event);
        }
        if (type == "error") {
            closeBlock();
            const bool aborted = event.contains("aborted") && event["aborted"].is_boolean() && event["aborted"].get<bool>();
            m_emitter.error(aborted ? StopReason::Aborted : StopReason::Error, text(event, "message", "the provider failed"));
            m_finished = true;
            return false;
        }
        return fail("the plugin sent an unknown event type \"" + type + "\"");
    }

    /** After the plugin's function returned: ends a response it left open. */
    void finish() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_finished) {
            return;
        }
        if (m_signal && m_signal->aborted()) {
            abort();
            return;
        }
        fail("the plugin's stream function returned without a done or error event");
    }

private:
    AssistantMessage initial(const Model& model, std::int64_t timestamp) const {
        AssistantMessage message;
        message.api = model.api;
        message.provider = model.provider;
        message.model = model.id;
        message.timestamp = timestamp;
        return message;
    }

    bool delta(bool isText, const Json& event) {
        if (!event.contains("delta") || !event["delta"].is_string()) {
            return fail("a delta event needs a string \"delta\"");
        }
        if (isText ? !m_textOpen : !m_thinkingOpen) {
            closeBlock();
            m_index = isText ? m_emitter.textStart() : m_emitter.thinkingStart();
            (isText ? m_textOpen : m_thinkingOpen) = true;
        }
        if (isText) {
            m_emitter.textDelta(m_index, event["delta"].get<std::string>());
        } else {
            m_emitter.thinkingDelta(m_index, event["delta"].get<std::string>());
        }
        return true;
    }

    bool toolCall(const Json& event) {
        if (!event.contains("id") || !event["id"].is_string() || !event.contains("name") || !event["name"].is_string() || (event.contains("arguments") && !event["arguments"].is_object())) {
            return fail("a tool_call event needs a string id and name and object arguments");
        }
        closeBlock();
        const int index = m_emitter.toolCallStart(event["id"].get<std::string>(), event["name"].get<std::string>());
        m_emitter.toolCallDelta(index, event.contains("arguments") ? event["arguments"].dump() : "{}");
        m_emitter.toolCallEnd(index);
        m_calledTool = true;
        return true;
    }

    bool usage(const Json& event) {
        Usage& usage = m_emitter.message().usage;
        usage.input = count(event, "input");
        usage.output = count(event, "output");
        usage.cacheRead = count(event, "cacheRead");
        usage.cacheWrite = count(event, "cacheWrite");
        usage.totalTokens = usage.input + usage.output + usage.cacheRead + usage.cacheWrite;
        CostCalculator().calculate(m_model, usage);
        return true;
    }

    bool response(const Json& event) {
        AssistantMessage& message = m_emitter.message();
        if (event.contains("id") && event["id"].is_string()) {
            message.responseId = event["id"].get<std::string>();
        }
        if (event.contains("model") && event["model"].is_string() && event["model"].get<std::string>() != message.model) {
            message.responseModel = event["model"].get<std::string>();
        }
        return true;
    }

    bool done(const Json& event) {
        StopReason reason = m_calledTool ? StopReason::ToolUse : StopReason::Stop;
        const std::string given = text(event, "stopReason", "");
        if (given == "length") {
            reason = StopReason::Length;
        } else if (given == "toolUse") {
            reason = StopReason::ToolUse;
        } else if (given == "stop") {
            reason = StopReason::Stop;
        } else if (!given.empty()) {
            return fail("done has an unknown stopReason \"" + given + "\"");
        }
        closeBlock();
        m_emitter.done(reason);
        m_finished = true;
        return false;
    }

    bool abort() {
        closeBlock();
        m_emitter.error(StopReason::Aborted, "Request was aborted");
        m_finished = true;
        return false;
    }

    bool fail(const std::string& message) {
        closeBlock();
        m_emitter.error(StopReason::Error, message);
        m_finished = true;
        return false;
    }

    void closeBlock() {
        if (m_textOpen) {
            m_emitter.textEnd(m_index);
        } else if (m_thinkingOpen) {
            m_emitter.thinkingEnd(m_index);
        }
        m_textOpen = false;
        m_thinkingOpen = false;
    }

    std::int64_t count(const Json& event, const std::string& key) const {
        return event.contains(key) && event[key].is_number_integer() ? event[key].get<std::int64_t>() : 0;
    }

    std::string text(const Json& event, const std::string& key, const std::string& fallback) const {
        return event.contains(key) && event[key].is_string() ? event[key].get<std::string>() : fallback;
    }

    Model m_model;
    std::shared_ptr<AbortSignal> m_signal;
    AssistantStreamEmitter m_emitter;
    std::mutex m_mutex;
    bool m_textOpen = false;
    bool m_thinkingOpen = false;
    int m_index = 0;
    bool m_started = false;
    bool m_finished = false;
    bool m_calledTool = false;
};
