export module pi.types.agent_loop_config;

import std;
export import pi.provider.i_provider;
export import pi.types.after_tool_call_result;
export import pi.types.agent_context;
export import pi.types.agent_loop_turn_update;
export import pi.types.agent_message;
export import pi.types.agent_turn_action;
export import pi.types.agent_turn_context;
export import pi.types.before_tool_call_result;
export import pi.types.model;
export import pi.types.prepare_request_context;
export import pi.types.stream_options;
export import pi.types.tool_call_context;
export import pi.types.tool_execution_mode;
export import pi.types.transcript_context;

/** Starts a provider request; never throws (failures arrive as stream error events). */
export using StreamFn = std::function<std::shared_ptr<AssistantMessageStream>(
    const Model&, const TranscriptContext&, const StreamOptions&)>;

/**
 * Configuration of one agent-loop run. All hooks are optional except convertToLlm and must
 * not throw. Port of AgentLoopConfig (packages/agent/src/types.ts).
 */
export struct AgentLoopConfig {
    Model model;
    /** Request options; `reasoning` is the thinking level of the run. */
    StreamOptions options;

    /** Maps agent messages to LLM messages before each request; drops UI-only entries. */
    std::function<std::vector<Message>(const std::vector<AgentMessage>&)> convertToLlm;
    std::function<std::vector<AgentMessage>(const std::vector<AgentMessage>&,
                                            const std::shared_ptr<AbortSignal>&)>
        transformContext;
    /** Resolves an API key per request (short-lived OAuth tokens). */
    std::function<std::optional<std::string>(const std::string& provider)> getApiKey;

    /** Called after a turn's tool results, before turn_end. nullopt keeps normal scheduling. */
    std::function<std::optional<AgentTurnAction>(const AgentTurnContext&,
                                                 const std::shared_ptr<AbortSignal>&)>
        finishTurn;
    /** Called immediately before every provider request; may replace context/model/level. */
    std::function<std::optional<AgentLoopTurnUpdate>(const PrepareRequestContext&,
                                                     const std::shared_ptr<AbortSignal>&)>
        prepareRequest;
    /** Called after turn_end when the loop continues, before the next turn starts. */
    std::function<std::optional<AgentLoopTurnUpdate>(const AgentTurnContext&)> prepareNextTurn;
    /** Steering messages injected mid-run, polled after each turn's tool calls. */
    std::function<std::vector<AgentMessage>()> getSteeringMessages;
    /** Follow-up messages processed when the agent would otherwise stop. */
    std::function<std::vector<AgentMessage>()> getFollowUpMessages;

    ToolExecutionMode toolExecution = ToolExecutionMode::Parallel;

    /** Runs after argument validation; may block the call. */
    std::function<std::optional<BeforeToolCallResult>(const ToolCallContext&,
                                                      const std::shared_ptr<AbortSignal>&)>
        beforeToolCall;
    /** Runs after execution, before result events; may override parts of the result. */
    std::function<std::optional<AfterToolCallResult>(const ToolCallContext&,
                                                     const std::shared_ptr<AbortSignal>&)>
        afterToolCall;
};
