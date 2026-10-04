export module pi.testing.recording_session_sink;

import std;
export import pi.session.i_session_event_sink;

/** ISessionEventSink that keeps every event for assertions. */
export class RecordingSessionSink : public ISessionEventSink {
public:
    void emit(const AgentSessionEvent& event) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_events.push_back(event);
    }

    std::vector<AgentSessionEvent> events() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_events;
    }

    std::vector<AgentSessionEvent> eventsOf(SessionEventType type) const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<AgentSessionEvent> out;
        for (const auto& event : m_events) {
            if (event.type == type) {
                out.push_back(event);
            }
        }
        return out;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<AgentSessionEvent> m_events;
};
