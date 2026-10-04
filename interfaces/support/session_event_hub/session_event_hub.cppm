module;

#include <cstdint>

export module pi.support.session_event_hub;

import std;
export import pi.session.i_session_event_sink;

/**
 * ISessionEventSink that fans events out to subscribers in subscription order. Listeners run on
 * the emitting thread, outside the hub's lock, so a listener may subscribe or unsubscribe.
 */
export class SessionEventHub : public ISessionEventSink {
public:
    using Listener = std::function<void(const AgentSessionEvent&)>;
    using ListenerId = std::uint64_t;

    ListenerId subscribe(Listener listener) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const ListenerId id = m_next++;
        m_listeners.emplace(id, std::move(listener));
        return id;
    }

    void unsubscribe(ListenerId id) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_listeners.erase(id);
    }

    void clear() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_listeners.clear();
    }

    void emit(const AgentSessionEvent& event) override {
        std::vector<Listener> snapshot;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& [id, listener] : m_listeners) {
                snapshot.push_back(listener);
            }
        }
        for (const Listener& listener : snapshot) {
            listener(event);
        }
    }

private:
    std::mutex m_mutex;
    std::map<ListenerId, Listener> m_listeners;
    ListenerId m_next = 1;
};
