export module pi.support.pending_input_tracker;

import std;
export import pi.session.i_session_event_sink;
export import pi.types.queued_input;

/**
 * Remembers the texts of steering and follow-up messages waiting for delivery, so clients can
 * show them and take them back, and announces every change as a queue_update event.
 */
export class PendingInputTracker {
public:
    explicit PendingInputTracker(ISessionEventSink& sink);

    void queueSteering(const std::string& text);
    void queueFollowUp(const std::string& text);

    /** A user message started: forget its text (steering queue first, then follow-up). */
    void delivered(const std::string& text);

    /** Forgets everything and returns what was waiting. */
    QueuedInput clear();

    QueuedInput snapshot() const;
    std::size_t count() const;

private:
    void emitUpdate(const QueuedInput& state);

    ISessionEventSink& m_sink;
    mutable std::mutex m_mutex;
    QueuedInput m_state;
};

PendingInputTracker::PendingInputTracker(ISessionEventSink& sink) : m_sink(sink) {}

void PendingInputTracker::emitUpdate(const QueuedInput& state) {
    AgentSessionEvent event;
    event.type = SessionEventType::QueueUpdate;
    event.steering = state.steering;
    event.followUp = state.followUp;
    m_sink.emit(event);
}

void PendingInputTracker::queueSteering(const std::string& text) {
    QueuedInput copy;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_state.steering.push_back(text);
        copy = m_state;
    }
    emitUpdate(copy);
}

void PendingInputTracker::queueFollowUp(const std::string& text) {
    QueuedInput copy;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_state.followUp.push_back(text);
        copy = m_state;
    }
    emitUpdate(copy);
}

void PendingInputTracker::delivered(const std::string& text) {
    if (text.empty()) {
        return;
    }
    QueuedInput copy;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto steering = std::ranges::find(m_state.steering, text);
        if (steering != m_state.steering.end()) {
            m_state.steering.erase(steering);
        } else if (const auto followUp = std::ranges::find(m_state.followUp, text); followUp != m_state.followUp.end()) {
            m_state.followUp.erase(followUp);
        } else {
            return;
        }
        copy = m_state;
    }
    emitUpdate(copy);
}

QueuedInput PendingInputTracker::clear() {
    QueuedInput previous;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        previous = std::exchange(m_state, QueuedInput{});
    }
    emitUpdate(QueuedInput{});
    return previous;
}

QueuedInput PendingInputTracker::snapshot() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

std::size_t PendingInputTracker::count() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_state.steering.size() + m_state.followUp.size();
}
