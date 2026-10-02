module;

#include <nlohmann/json.hpp>

export module pi.types.prepared_tool_call;

import std;
export import pi.tool.i_tool;
export import pi.types.agent_tool_result;
export import pi.types.json;
export import pi.types.tool_call;

/**
 * A tool call after lookup, argument preparation, validation and beforeToolCall. Either ready
 * to execute (`tool` set) or already settled (`immediate`, e.g. unknown tool or blocked call).
 */
export struct PreparedToolCall {
    ToolCall toolCall;
    std::shared_ptr<ITool> tool;
    Json args;
    bool immediate = false;
    AgentToolResult immediateResult;
    bool immediateIsError = false;
};
