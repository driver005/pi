#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "interfaces/support/abort_signal/abort_signal.h"
#include "interfaces/tool/i_tool/i_tool.h"
#include "interfaces/types/agent_event/agent_event.h"
#include "interfaces/types/agent_message/agent_message.h"
#include "interfaces/types/agent_state/agent_state.h"
#include "interfaces/types/image_content/image_content.h"
#include "interfaces/types/model/model.h"
#include "interfaces/types/queue_mode/queue_mode.h"
#include "interfaces/types/result/result.h"
#include "interfaces/types/thinking_level/thinking_level.h"

/**
 * Stateful agent: owns the transcript, runs the loop, queues steering and follow-up messages.
 * One run at a time. prompt() and continueRun() block until the run (and all listeners for its
 * final event) finished, so call them from a worker thread; every other method is thread-safe.
 */
class IAgent {
public:
    using Listener = std::function<void(const AgentEvent&, const std::shared_ptr<AbortSignal>&)>;
    using ListenerId = std::uint64_t;

    virtual ~IAgent() = default;

    virtual AgentState state() const = 0;
    virtual void setModel(const Model& model) = 0;
    virtual void setThinkingLevel(ThinkingLevel level) = 0;
    virtual void setTools(std::vector<std::shared_ptr<ITool>> tools) = 0;
    virtual void setMessages(std::vector<AgentMessage> messages) = 0;

    /** Listeners are called in subscription order, serially, from the thread running the loop. */
    virtual ListenerId subscribe(Listener listener) = 0;
    virtual void unsubscribe(ListenerId id) = 0;

    virtual void setSteeringMode(QueueMode mode) = 0;
    virtual void setFollowUpMode(QueueMode mode) = 0;
    /** Queue a message injected after the current assistant turn finishes. */
    virtual void steer(AgentMessage message) = 0;
    /** Queue a message processed only after the agent would otherwise stop. */
    virtual void followUp(AgentMessage message) = 0;
    virtual void clearSteeringQueue() = 0;
    virtual void clearFollowUpQueue() = 0;
    virtual void clearAllQueues() = 0;
    virtual bool hasQueuedMessages() const = 0;
    virtual std::vector<AgentMessage> peekQueuedMessages() const = 0;

    /** Starts a run from messages; Error if a run is already active. */
    virtual Result<void> prompt(std::vector<AgentMessage> messages) = 0;
    /** Starts a run from user text and optional images. */
    virtual Result<void> promptText(const std::string& text, const std::vector<ImageContent>& images) = 0;
    /** Continues from the transcript; the last message must be a user or tool-result message. */
    virtual Result<void> continueRun() = 0;

    virtual void abort() = 0;
    virtual void waitForIdle() = 0;
    /** Clears conversation state and queues, keeping the replayed prompt/tool baseline. */
    virtual Result<void> reset() = 0;
};
