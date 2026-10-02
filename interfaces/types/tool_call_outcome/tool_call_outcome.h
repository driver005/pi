#pragma once

#include "interfaces/types/agent_tool_result/agent_tool_result.h"
#include "interfaces/types/tool_call/tool_call.h"

/** Final outcome of one tool call after the hooks ran. */
struct ToolCallOutcome {
    ToolCall toolCall;
    AgentToolResult result;
    bool isError = false;
};
