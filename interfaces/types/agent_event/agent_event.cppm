module;

#include <nlohmann/json.hpp>

export module pi.types.agent_event;

import std;
export import pi.types.agent_message;
export import pi.types.agent_tool_result;
export import pi.types.assistant_message_event;
export import pi.types.json;
export import pi.types.tool_result_message;

export enum class AgentEventType {
    AgentStart,
    AgentEnd,
    TurnStart,
    TurnEnd,
    MessageStart,
    MessageUpdate,
    MessageEnd,
    ToolExecutionStart,
    ToolExecutionUpdate,
    ToolExecutionEnd
};

/**
 * Event emitted by the agent loop. Which fields are set depends on `type`:
 * AgentEnd: messages. TurnEnd: message + toolResults. MessageStart/End: message.
 * MessageUpdate: message + assistantEvent. ToolExecutionStart: toolCallId, toolName, args.
 * ToolExecutionUpdate: those + result (partial). ToolExecutionEnd: toolCallId, toolName,
 * result, isError.
 */
export struct AgentEvent {
    AgentEventType type = AgentEventType::AgentStart;
    std::vector<AgentMessage> messages;
    std::shared_ptr<const AgentMessage> message;
    std::vector<ToolResultMessage> toolResults;
    std::shared_ptr<const AssistantMessageEvent> assistantEvent;
    std::string toolCallId;
    std::string toolName;
    Json args;
    std::shared_ptr<const AgentToolResult> result;
    bool isError = false;
};
