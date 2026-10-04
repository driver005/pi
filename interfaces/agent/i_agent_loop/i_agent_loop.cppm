export module pi.agent.i_agent_loop;

import std;
export import pi.support.abort_signal;
export import pi.types.agent_context;
export import pi.types.agent_event_sink;
export import pi.types.agent_loop_config;
export import pi.types.agent_message;
export import pi.types.result;

/** The agent loop: prompt -> provider -> tool calls -> provider ... until done. Blocking. */
export class IAgentLoop {
public:
    virtual ~IAgentLoop() = default;

    /**
     * Starts a run with new prompt messages appended to the context. Emits events through
     * `emit` and returns the messages created during the run.
     */
    virtual std::vector<AgentMessage> run(std::vector<AgentMessage> prompts, AgentContext context,
                                          const AgentLoopConfig& config, const AgentEventSink& emit,
                                          const std::shared_ptr<AbortSignal>& signal,
                                          const StreamFn& streamFn) = 0;

    /**
     * Continues from the current context without adding a message (retries). The last message
     * must convert to a user or toolResult message. Error if the context is empty or ends with
     * an assistant message.
     */
    virtual Result<std::vector<AgentMessage>> runContinue(AgentContext context,
                                                          const AgentLoopConfig& config,
                                                          const AgentEventSink& emit,
                                                          const std::shared_ptr<AbortSignal>& signal,
                                                          const StreamFn& streamFn) = 0;
};
