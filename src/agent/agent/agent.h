#pragma once

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "interfaces/agent/i_agent/i_agent.h"
#include "interfaces/agent/i_agent_loop/i_agent_loop.h"
#include "interfaces/platform/i_clock/i_clock.h"
#include "interfaces/support/pending_message_queue/pending_message_queue.h"
#include "interfaces/support/transcript_normalizer/transcript_normalizer.h"
#include "interfaces/types/agent_options/agent_options.h"

/** Stateful wrapper around IAgentLoop (port of packages/agent/src/agent.ts). */
class Agent : public IAgent {
public:
    Agent(AgentOptions options, IAgentLoop& loop, const IClock& clock);

    AgentState state() const override;
    void setModel(const Model& model) override;
    void setThinkingLevel(ThinkingLevel level) override;
    void setTools(std::vector<std::shared_ptr<ITool>> tools) override;
    void setMessages(std::vector<AgentMessage> messages) override;

    ListenerId subscribe(Listener listener) override;
    void unsubscribe(ListenerId id) override;

    void setSteeringMode(QueueMode mode) override;
    void setFollowUpMode(QueueMode mode) override;
    void steer(AgentMessage message) override;
    void followUp(AgentMessage message) override;
    void clearSteeringQueue() override;
    void clearFollowUpQueue() override;
    void clearAllQueues() override;
    bool hasQueuedMessages() const override;
    std::vector<AgentMessage> peekQueuedMessages() const override;

    Result<void> prompt(std::vector<AgentMessage> messages) override;
    Result<void> promptText(const std::string& text, const std::vector<ImageContent>& images) override;
    Result<void> continueRun() override;

    void abort() override;
    void waitForIdle() override;
    Result<void> reset() override;

private:
    std::vector<Message> llmMessagesLocked() const;
    AgentLoopConfig createLoopConfig(bool skipInitialSteeringPoll);
    Result<void> runWithLifecycle(bool isContinuation, std::vector<AgentMessage> prompts,
                                  bool skipInitialSteeringPoll);
    void finishRun();
    void processEvent(const AgentEvent& event);
    void reduceEvent(const AgentEvent& event);
    Result<void> beginRun(std::shared_ptr<AbortSignal>& signal);

    IAgentLoop& m_loop;
    const IClock& m_clock;
    TranscriptNormalizer m_transcript;
    AgentOptions m_options;
    PendingMessageQueue m_steering;
    PendingMessageQueue m_followUps;

    mutable std::mutex m_mutex;
    std::condition_variable m_idle;
    Model m_model;
    ThinkingLevel m_thinkingLevel;
    std::vector<std::shared_ptr<ITool>> m_tools;
    std::vector<AgentMessage> m_messages;
    bool m_running = false;
    std::shared_ptr<AbortSignal> m_signal;
    std::optional<AgentMessage> m_streamingMessage;
    std::set<std::string> m_pendingToolCalls;
    std::optional<std::string> m_errorMessage;
    std::map<ListenerId, Listener> m_listeners;
    ListenerId m_nextListener = 1;
};
