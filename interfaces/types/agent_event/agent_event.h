#pragma once

#include <memory>
#include <string>
#include <vector>

#include "interfaces/types/agent_message/agent_message.h"
#include "interfaces/types/agent_tool_result/agent_tool_result.h"
#include "interfaces/types/assistant_message_event/assistant_message_event.h"
#include "interfaces/types/json/json.h"
#include "interfaces/types/tool_result_message/tool_result_message.h"

enum class AgentEventType {
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
struct AgentEvent {
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
