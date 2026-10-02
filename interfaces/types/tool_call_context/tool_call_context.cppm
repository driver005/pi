module;

#include <nlohmann/json.hpp>

export module pi.types.tool_call_context;

import std;
export import pi.types.agent_context;
export import pi.types.agent_tool_result;
export import pi.types.assistant_message;
export import pi.types.json;
export import pi.types.tool_call;

/**
 * What beforeToolCall and afterToolCall see. `result` and `isError` are only set for
 * afterToolCall. Pointers stay valid for the duration of the hook call.
 */
export struct ToolCallContext {
    const AssistantMessage* assistantMessage = nullptr;
    const ToolCall* toolCall = nullptr;
    /** Validated (and coerced) arguments. */
    Json args;
    const AgentContext* context = nullptr;
    std::optional<AgentToolResult> result;
    bool isError = false;
};
