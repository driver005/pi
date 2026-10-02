#pragma once

#include <vector>

#include "interfaces/types/agent_context/agent_context.h"
#include "interfaces/types/agent_message/agent_message.h"
#include "interfaces/types/assistant_message/assistant_message.h"
#include "interfaces/types/tool_result_message/tool_result_message.h"

/** A completed turn: the assistant message, its tool results and the resulting context. */
struct AgentTurnContext {
    AssistantMessage message;
    std::vector<ToolResultMessage> toolResults;
    AgentContext context;
    /** Messages this loop invocation returns if it exits at this point. */
    std::vector<AgentMessage> newMessages;
};
