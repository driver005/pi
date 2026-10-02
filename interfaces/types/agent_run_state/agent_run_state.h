#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "interfaces/support/abort_signal/abort_signal.h"
#include "interfaces/types/agent_context/agent_context.h"
#include "interfaces/types/agent_event_sink/agent_event_sink.h"
#include "interfaces/types/agent_loop_config/agent_loop_config.h"
#include "interfaces/types/agent_message/agent_message.h"
#include "interfaces/types/agent_turn_context/agent_turn_context.h"

/** Mutable state of one agent-loop run, owned by the loop for the duration of the run. */
struct AgentRunState {
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
