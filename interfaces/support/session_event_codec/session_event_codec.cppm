module;

#include <nlohmann/json.hpp>

export module pi.support.session_event_codec;

import std;
export import pi.support.agent_message_codec;
export import pi.support.message_codec;
export import pi.support.session_data_codec;
export import pi.types.agent_session_event;
export import pi.types.json;

/**
 * Wire JSON of session events, as the JSONL protocols stream them. Assistant message updates drop
 * the cumulative `partial` snapshot (the start event, the deltas and the final message carry the
 * same information); tool call starts gain the call's id and name instead.
 */
export class SessionEventCodec {
public:
    Json toJson(const AgentSessionEvent& event) const;

private:
    Json agentEvent(const AgentEvent& event, bool withRetry, bool willRetry) const;
    Json messageUpdate(const AgentEvent& event) const;
    Json assistantEvent(const AssistantMessageEvent& event) const;
    Json toolExecution(const AgentEvent& event, const char* type) const;
    Json sessionEvent(const AgentSessionEvent& event) const;
    Json compactionEnd(const AgentSessionEvent& event) const;
    Json retryEvent(const AgentSessionEvent& event) const;
    Json summarizationEvent(const AgentSessionEvent& event) const;
    std::string reasonName(StopReason reason) const;

    AgentMessageCodec m_agentMessages;
    MessageCodec m_messages;
    SessionDataCodec m_data;
};

std::string SessionEventCodec::reasonName(StopReason reason) const {
    return m_messages.stopReasonName(reason);
}

Json SessionEventCodec::assistantEvent(const AssistantMessageEvent& event) const {
    Json out = Json::object();
    switch (event.type) {
    case AssistantEventType::Start:
        out["type"] = "start";
        return out;
    case AssistantEventType::TextStart:
    case AssistantEventType::ThinkingStart:
        out["type"] = event.type == AssistantEventType::TextStart ? "text_start" : "thinking_start";
        break;
    case AssistantEventType::TextDelta:
    case AssistantEventType::ThinkingDelta:
    case AssistantEventType::ToolCallDelta:
        out["type"] = event.type == AssistantEventType::TextDelta       ? "text_delta"
                      : event.type == AssistantEventType::ThinkingDelta ? "thinking_delta"
                                                                        : "toolcall_delta";
        out["contentIndex"] = event.contentIndex;
        out["delta"] = event.text;
        return out;
    case AssistantEventType::TextEnd:
    case AssistantEventType::ThinkingEnd:
        out["type"] = event.type == AssistantEventType::TextEnd ? "text_end" : "thinking_end";
        out["contentIndex"] = event.contentIndex;
        out["content"] = event.text;
        return out;
    case AssistantEventType::ToolCallStart:
        out["type"] = "toolcall_start";
        out["contentIndex"] = event.contentIndex;
        if (event.partial && event.contentIndex >= 0 &&
            static_cast<std::size_t>(event.contentIndex) < event.partial->content.size()) {
            if (const auto* call = std::get_if<ToolCall>(&event.partial->content[static_cast<std::size_t>(event.contentIndex)])) {
                out["id"] = call->id;
                out["toolName"] = call->name;
            }
        }
        return out;
    case AssistantEventType::ToolCallEnd:
        out["type"] = "toolcall_end";
        out["contentIndex"] = event.contentIndex;
        if (event.toolCall) {
            out["toolCall"] = m_messages.toJson(*event.toolCall);
        }
        return out;
    case AssistantEventType::Done:
        out["type"] = "done";
        out["reason"] = reasonName(event.reason);
        if (event.message) {
            out["message"] = m_messages.toJson(*event.message);
        }
        return out;
    case AssistantEventType::Error:
        out["type"] = "error";
        out["reason"] = reasonName(event.reason);
        if (event.message) {
            out["error"] = m_messages.toJson(*event.message);
        }
        return out;
    }
    return out;
}

Json SessionEventCodec::messageUpdate(const AgentEvent& event) const {
    Json out = Json::object();
    out["type"] = "message_update";
    if (event.message != nullptr) {
        if (const auto* assistant = std::get_if<AssistantMessage>(event.message.get())) {
            out["usage"] = m_messages.toJson(assistant->usage);
        }
    }
    if (event.assistantEvent) {
        out["assistantMessageEvent"] = assistantEvent(*event.assistantEvent);
    }
    return out;
}

Json SessionEventCodec::toolExecution(const AgentEvent& event, const char* type) const {
    Json out = Json::object();
    out["type"] = type;
    out["toolCallId"] = event.toolCallId;
    out["toolName"] = event.toolName;
    if (event.type != AgentEventType::ToolExecutionEnd) {
        out["args"] = event.args;
    }
    if (event.type == AgentEventType::ToolExecutionUpdate && event.result) {
        out["partialResult"] = m_data.toolResult(*event.result);
    }
    if (event.type == AgentEventType::ToolExecutionEnd) {
        if (event.result) {
            out["result"] = m_data.toolResult(*event.result);
        }
        out["isError"] = event.isError;
    }
    return out;
}

Json SessionEventCodec::agentEvent(const AgentEvent& event, bool withRetry, bool willRetry) const {
    Json out = Json::object();
    switch (event.type) {
    case AgentEventType::AgentStart:
        out["type"] = "agent_start";
        break;
    case AgentEventType::AgentEnd:
        out["type"] = "agent_end";
        out["messages"] = m_agentMessages.listToJson(event.messages);
        if (withRetry) {
            out["willRetry"] = willRetry;
        }
        break;
    case AgentEventType::TurnStart:
        out["type"] = "turn_start";
        break;
    case AgentEventType::TurnEnd: {
        out["type"] = "turn_end";
        if (event.message) {
            out["message"] = m_agentMessages.toJson(*event.message);
        }
        Json results = Json::array();
        for (const auto& result : event.toolResults) {
            results.push_back(m_messages.toJson(result));
        }
        out["toolResults"] = results;
        break;
    }
    case AgentEventType::MessageStart:
    case AgentEventType::MessageEnd:
        out["type"] = event.type == AgentEventType::MessageStart ? "message_start" : "message_end";
        if (event.message) {
            out["message"] = m_agentMessages.toJson(*event.message);
        }
        break;
    case AgentEventType::MessageUpdate:
        return messageUpdate(event);
    case AgentEventType::ToolExecutionStart:
        return toolExecution(event, "tool_execution_start");
    case AgentEventType::ToolExecutionUpdate:
        return toolExecution(event, "tool_execution_update");
    case AgentEventType::ToolExecutionEnd:
        return toolExecution(event, "tool_execution_end");
    }
    return out;
}

Json SessionEventCodec::compactionEnd(const AgentSessionEvent& event) const {
    Json out = Json::object();
    out["type"] = "compaction_end";
    out["reason"] = event.reason;
    if (event.result) {
        out["result"] = m_data.compaction(*event.result);
    }
    out["aborted"] = event.aborted;
    out["willRetry"] = event.willRetry;
    if (event.errorMessage) {
        out["errorMessage"] = *event.errorMessage;
    }
    return out;
}

Json SessionEventCodec::retryEvent(const AgentSessionEvent& event) const {
    Json out = Json::object();
    if (event.type == SessionEventType::AutoRetryStart) {
        out = Json{{"type", "auto_retry_start"},
                   {"attempt", event.attempt},
                   {"maxAttempts", event.maxAttempts},
                   {"delayMs", event.delayMs},
                   {"errorMessage", event.errorMessage.value_or("")}};
        return out;
    }
    out = Json{{"type", "auto_retry_end"}, {"success", event.success}, {"attempt", event.attempt}};
    if (event.finalError) {
        out["finalError"] = *event.finalError;
    }
    return out;
}

Json SessionEventCodec::summarizationEvent(const AgentSessionEvent& event) const {
    if (event.type == SessionEventType::SummarizationRetryScheduled) {
        return Json{{"type", "summarization_retry_scheduled"},
                    {"attempt", event.attempt},
                    {"maxAttempts", event.maxAttempts},
                    {"delayMs", event.delayMs},
                    {"errorMessage", event.errorMessage.value_or("")}};
    }
    if (event.type == SessionEventType::SummarizationRetryFinished) {
        return Json{{"type", "summarization_retry_finished"}};
    }
    Json out = Json{{"type", "summarization_retry_attempt_start"}, {"source", event.source}};
    if (event.source == "compaction") {
        out["reason"] = event.reason;
    }
    return out;
}

Json SessionEventCodec::sessionEvent(const AgentSessionEvent& event) const {
    switch (event.type) {
    case SessionEventType::AgentSettled:
        return Json{{"type", "agent_settled"}};
    case SessionEventType::QueueUpdate:
        return Json{{"type", "queue_update"},
                    {"steering", m_data.strings(event.steering)},
                    {"followUp", m_data.strings(event.followUp)}};
    case SessionEventType::CompactionStart:
        return Json{{"type", "compaction_start"}, {"reason", event.reason}};
    case SessionEventType::CompactionEnd:
        return compactionEnd(event);
    case SessionEventType::EntryAppended:
        return Json{{"type", "entry_appended"}, {"entry", event.entry ? event.entry->body : Json(nullptr)}};
    case SessionEventType::SessionInfoChanged: {
        Json out = Json{{"type", "session_info_changed"}};
        out["name"] = m_data.optionalString(event.name);
        return out;
    }
    case SessionEventType::ThinkingLevelChanged:
        return Json{{"type", "thinking_level_changed"}, {"level", m_messages.thinkingLevelName(event.level)}};
    case SessionEventType::AutoRetryStart:
    case SessionEventType::AutoRetryEnd:
        return retryEvent(event);
    case SessionEventType::SummarizationRetryScheduled:
    case SessionEventType::SummarizationRetryAttemptStart:
    case SessionEventType::SummarizationRetryFinished:
        return summarizationEvent(event);
    case SessionEventType::BashExecutionUpdate: {
        Json out = Json{{"type", "bash_execution_update"}, {"delta", event.delta}};
        if (event.id) {
            out["id"] = *event.id;
        }
        return out;
    }
    default:
        return Json::object();
    }
}

Json SessionEventCodec::toJson(const AgentSessionEvent& event) const {
    if (event.type == SessionEventType::Agent && event.agent) {
        return agentEvent(*event.agent, false, false);
    }
    if (event.type == SessionEventType::AgentEnd && event.agent) {
        return agentEvent(*event.agent, true, event.willRetry);
    }
    return sessionEvent(event);
}
