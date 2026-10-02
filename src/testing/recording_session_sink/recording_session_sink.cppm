export module pi.testing.recording_session_sink;

import std;
export import pi.session.i_session_event_sink;

/** ISessionEventSink that keeps every event for assertions. */
export class RecordingSessionSink : public ISessionEventSink {
public:
    void emit(const AgentSessionEvent& event) override;

    std::vector<AgentSessionEvent> events() const;
    std::vector<AgentSessionEvent> eventsOf(SessionEventType type) const;

private:
    mutable std::mutex m_mutex;
    std::vector<AgentSessionEvent> m_events;
};

void RecordingSessionSink::emit(const AgentSessionEvent& event) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_events.push_back(event);
}

std::vector<AgentSessionEvent> RecordingSessionSink::events() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_events;
}

std::vector<AgentSessionEvent> RecordingSessionSink::eventsOf(SessionEventType type) const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<AgentSessionEvent> out;
    for (const auto& event : m_events) {
        if (event.type == type) {
            out.push_back(event);
        }
    }
    return out;
}
