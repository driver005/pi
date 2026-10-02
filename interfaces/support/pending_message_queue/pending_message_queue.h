#pragma once

#include <mutex>
#include <vector>

#include "interfaces/types/agent_message/agent_message.h"
#include "interfaces/types/queue_mode/queue_mode.h"

/** Thread-safe FIFO of messages awaiting injection into a running agent. */
class PendingMessageQueue {
public:
    explicit PendingMessageQueue(QueueMode mode);

    void enqueue(AgentMessage message);
    bool hasItems() const;
    /** The messages the next drain would return, without consuming them. */
    std::vector<AgentMessage> peek() const;
    /** All queued messages (All) or only the oldest (OneAtATime), removed from the queue. */
    std::vector<AgentMessage> drain();
    void clear();
    QueueMode mode() const;
    void setMode(QueueMode mode);

private:
    std::vector<AgentMessage> selectLocked() const;

    mutable std::mutex m_mutex;
    std::vector<AgentMessage> m_messages;
    QueueMode m_mode;
};
