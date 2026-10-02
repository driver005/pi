export module pi.types.agent_run_state;

import std;
export import pi.support.abort_signal;
export import pi.types.agent_context;
export import pi.types.agent_event_sink;
export import pi.types.agent_loop_config;
export import pi.types.agent_message;
export import pi.types.agent_turn_context;

/** Mutable state of one agent-loop run, owned by the loop for the duration of the run. */
export struct AgentRunState {
    AgentContext context;
    std::vector<AgentMessage> newMessages;
    AgentLoopConfig config;
    std::shared_ptr<AbortSignal> signal;
    AgentEventSink emit;
    StreamFn streamFn;
    std::optional<AgentTurnContext> lastCompletedTurn;
    bool explicitContinuation = false;
    /** Serializes emit() across concurrently finishing tool calls. */
    std::mutex emitMutex;
};
