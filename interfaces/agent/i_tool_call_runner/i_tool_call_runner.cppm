export module pi.agent.i_tool_call_runner;

import std;
export import pi.support.abort_signal;
export import pi.tool.i_tool;
export import pi.types.agent_context;
export import pi.types.agent_loop_config;
export import pi.types.assistant_message;
export import pi.types.prepared_tool_call;
export import pi.types.tool_call;
export import pi.types.tool_call_outcome;

/**
 * Runs one tool call through the same steps as a model-issued call: argument preparation,
 * schema validation, beforeToolCall, execution and afterToolCall. Never fails: unknown tools,
 * validation errors, blocked calls and tool errors all come back as outcomes with isError set.
 */
export class IToolCallRunner {
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
