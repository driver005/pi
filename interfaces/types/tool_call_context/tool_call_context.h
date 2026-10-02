#pragma once

#include <optional>

#include "interfaces/types/agent_context/agent_context.h"
#include "interfaces/types/agent_tool_result/agent_tool_result.h"
#include "interfaces/types/assistant_message/assistant_message.h"
#include "interfaces/types/json/json.h"
#include "interfaces/types/tool_call/tool_call.h"

/**
 * What beforeToolCall and afterToolCall see. `result` and `isError` are only set for
 * afterToolCall. Pointers stay valid for the duration of the hook call.
 */
struct ToolCallContext {
    const AssistantMessage* assistantMessage = nullptr;
    const ToolCall* toolCall = nullptr;
    /** Validated (and coerced) arguments. */
    Json args;
    const AgentContext* context = nullptr;
    std::optional<AgentToolResult> result;
    bool isError = false;
};
