module;

#include <cstdint>

export module pi.support.abort_signal;

import std;

/** Cooperative cancellation flag shared between a caller and long-running work. */
export class AbortSignal {
public:
    using Listener = std::function<void()>;

    bool aborted() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_aborted;
    }

    /** Marks the signal aborted and runs every listener once. Idempotent. */
    void abort() {
        std::vector<Listener> pending;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_aborted) {
                return;
            }
            m_aborted = true;
            for (auto& entry : m_listeners) {
                pending.push_back(std::move(entry.second));
            }
            m_listeners.clear();
        }
        for (const Listener& listener : pending) {
            listener();
        }
    }

    /** Registers a listener; runs it immediately if already aborted. Returns a removal id. */
    std::uint64_t onAbort(Listener listener) const {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_aborted) {
                const std::uint64_t id = m_nextId++;
                m_listeners.emplace(id, std::move(listener));
                return id;
            }
        }
        listener();
        return 0;
    }

    void removeListener(std::uint64_t id) const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_listeners.erase(id);
    }

private:
    mutable std::mutex m_mutex;
    bool m_aborted = false;
    mutable std::uint64_t m_nextId = 1;
    mutable std::map<std::uint64_t, Listener> m_listeners;
};
