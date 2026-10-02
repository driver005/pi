#pragma once

#include <memory>

#include "interfaces/support/abort_signal/abort_signal.h"
#include "interfaces/tool/i_tool/i_tool.h"
#include "interfaces/types/agent_context/agent_context.h"
#include "interfaces/types/agent_loop_config/agent_loop_config.h"
#include "interfaces/types/assistant_message/assistant_message.h"
#include "interfaces/types/prepared_tool_call/prepared_tool_call.h"
#include "interfaces/types/tool_call/tool_call.h"
#include "interfaces/types/tool_call_outcome/tool_call_outcome.h"

/**
 * Runs one tool call through the same steps as a model-issued call: argument preparation,
 * schema validation, beforeToolCall, execution and afterToolCall. Never fails: unknown tools,
 * validation errors, blocked calls and tool errors all come back as outcomes with isError set.
 */
class IToolCallRunner {
public:
    virtual ~IToolCallRunner() = default;

    /** Lookup, prepareArguments, validation and beforeToolCall (sequential part of a batch). */
    virtual PreparedToolCall prepare(const AgentContext& context, const AssistantMessage& assistant,
                                     const ToolCall& call, const AgentLoopConfig& hooks,
                                     const std::shared_ptr<AbortSignal>& signal) = 0;

    /** Executes a prepared call (or settles an immediate one) and applies afterToolCall. */
    virtual ToolCallOutcome execute(const AgentContext& context, const AssistantMessage& assistant,
                                    const PreparedToolCall& prepared, const AgentLoopConfig& hooks,
                                    const std::shared_ptr<AbortSignal>& signal,
                                    const ToolUpdateCallback& onUpdate) = 0;

    /** prepare() followed by execute(). */
    virtual ToolCallOutcome run(const AgentContext& context, const AssistantMessage& assistant,
                                const ToolCall& call, const AgentLoopConfig& hooks,
                                const std::shared_ptr<AbortSignal>& signal,
                                const ToolUpdateCallback& onUpdate) = 0;
};
