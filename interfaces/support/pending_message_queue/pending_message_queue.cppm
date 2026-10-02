export module pi.support.pending_message_queue;

import std;
export import pi.types.agent_message;
export import pi.types.queue_mode;

/** Thread-safe FIFO of messages awaiting injection into a running agent. */
export class PendingMessageQueue {
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

PendingMessageQueue::PendingMessageQueue(QueueMode mode) : m_mode(mode) {}

void PendingMessageQueue::enqueue(AgentMessage message) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_messages.push_back(std::move(message));
}

bool PendingMessageQueue::hasItems() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return !m_messages.empty();
}

std::vector<AgentMessage> PendingMessageQueue::selectLocked() const {
    if (m_mode == QueueMode::All || m_messages.empty()) {
        return m_messages;
    }
    return {m_messages.front()};
}

std::vector<AgentMessage> PendingMessageQueue::peek() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return selectLocked();
}

std::vector<AgentMessage> PendingMessageQueue::drain() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<AgentMessage> drained = selectLocked();
    m_messages.erase(m_messages.begin(), m_messages.begin() + static_cast<std::ptrdiff_t>(drained.size()));
    return drained;
}

void PendingMessageQueue::clear() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_messages.clear();
}

QueueMode PendingMessageQueue::mode() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_mode;
}

void PendingMessageQueue::setMode(QueueMode mode) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_mode = mode;
}
