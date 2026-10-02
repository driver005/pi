#pragma once

#include <memory>

#include "interfaces/tool/i_tool/i_tool.h"
#include "interfaces/types/agent_tool_result/agent_tool_result.h"
#include "interfaces/types/json/json.h"
#include "interfaces/types/tool_call/tool_call.h"

/**
 * A tool call after lookup, argument preparation, validation and beforeToolCall. Either ready
 * to execute (`tool` set) or already settled (`immediate`, e.g. unknown tool or blocked call).
 */
struct PreparedToolCall {
    ToolCall toolCall;
    std::shared_ptr<ITool> tool;
    Json args;
    bool immediate = false;
    AgentToolResult immediateResult;
    bool immediateIsError = false;
};
