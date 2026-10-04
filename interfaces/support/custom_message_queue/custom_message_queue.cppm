export module pi.support.custom_message_queue;

import std;
export import pi.session.i_session_event_sink;
export import pi.session.i_session_manager;
export import pi.support.session_context_refresher;
export import pi.support.session_message_persister;
export import pi.types.agent_message;

/**
 * Holds custom messages that must not enter the conversation yet: messages sent during a run wait
 * until the turn's tool results are in (so they never land between a tool call and its result),
 * and "next turn" messages ride along with the next user prompt.
 */
export class CustomMessageQueue {
public:
    CustomMessageQueue(ISessionManager& session, SessionContextRefresher& refresher, ISessionEventSink& sink)
        : m_refresher(refresher),
          m_sink(sink),
          m_persister(session) {}

    /** Waits for the end of the current turn. */
    void defer(const CustomMessage& message) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_deferred.push_back(message);
    }

    void queueForNextTurn(const CustomMessage& message) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_nextTurn.push_back(message);
    }

    std::vector<CustomMessage> takeNextTurn() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return std::exchange(m_nextTurn, {});
    }

    /** Stores the message, reloads the agent's context and announces it (message start and end). */
    void appendNow(const CustomMessage& message) {
        m_persister.persist(AgentMessage(message));
        m_refresher.refresh();
        announce(AgentEventType::MessageStart, message);
        announce(AgentEventType::MessageEnd, message);
    }

    /** Appends every deferred message. */
    void flush() {
        std::vector<CustomMessage> pending;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            pending.swap(m_deferred);
        }
        for (const auto& message : pending) {
            appendNow(message);
        }
    }

    bool hasDeferred() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return !m_deferred.empty();
    }

private:
    void announce(AgentEventType type, const CustomMessage& message) {
        auto event = std::make_shared<AgentEvent>();
        event->type = type;
        event->message = std::make_shared<const AgentMessage>(message);
        AgentSessionEvent out;
        out.type = SessionEventType::Agent;
        out.agent = std::move(event);
        m_sink.emit(out);
    }

    SessionContextRefresher& m_refresher;
    ISessionEventSink& m_sink;
    SessionMessagePersister m_persister;

    mutable std::mutex m_mutex;
    std::vector<CustomMessage> m_deferred;
    std::vector<CustomMessage> m_nextTurn;
};
