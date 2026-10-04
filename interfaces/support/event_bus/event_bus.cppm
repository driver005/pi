module;

#include <cstdint>

export module pi.support.event_bus;

import std;
export import pi.types.json;

/**
 * Named channels carrying JSON between plugins (port of core/event-bus.ts). Handlers run synchronously on the emitting thread in
 * subscription order; one that throws cannot, so there is nothing to catch. Thread-safe: handlers are copied out before they
 * run, so a handler may subscribe or unsubscribe.
 */
export class EventBus {
public:
    using Handler = std::function<void(const std::string&, const Json&)>;

    /** Returns an id for off(). */
    std::uint64_t on(const std::string& channel, Handler handler) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const std::uint64_t id = m_nextId++;
        m_handlers[channel].emplace_back(id, std::move(handler));
        return id;
    }

    void off(std::uint64_t id) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& entry : m_handlers) {
            std::erase_if(entry.second, [id](const std::pair<std::uint64_t, Handler>& handler) { return handler.first == id; });
        }
    }

    void emit(const std::string& channel, const Json& data) {
        std::vector<Handler> run;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_handlers.find(channel);
            if (found == m_handlers.end()) {
                return;
            }
            for (const auto& entry : found->second) {
                run.push_back(entry.second);
            }
        }
        for (const Handler& handler : run) {
            handler(channel, data);
        }
    }

    /** Drops every subscription. */
    void clear() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_handlers.clear();
    }

private:
    std::mutex m_mutex;
    std::map<std::string, std::vector<std::pair<std::uint64_t, Handler>>> m_handlers;
    std::uint64_t m_nextId = 1;
};
