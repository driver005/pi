export module pi.types.tool_call_outcome;

import std;
export import pi.types.agent_tool_result;
export import pi.types.tool_call;

/** Final outcome of one tool call after the hooks ran. */
export struct ToolCallOutcome {
    ToolCall toolCall;
    AgentToolResult result;
    bool isError = false;
};
