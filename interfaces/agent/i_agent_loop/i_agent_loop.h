#pragma once

#include <memory>
#include <vector>

#include "interfaces/support/abort_signal/abort_signal.h"
#include "interfaces/types/agent_context/agent_context.h"
#include "interfaces/types/agent_event_sink/agent_event_sink.h"
#include "interfaces/types/agent_loop_config/agent_loop_config.h"
#include "interfaces/types/agent_message/agent_message.h"
#include "interfaces/types/result/result.h"

/** The agent loop: prompt -> provider -> tool calls -> provider ... until done. Blocking. */
class IAgentLoop {
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
