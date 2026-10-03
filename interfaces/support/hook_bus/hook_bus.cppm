module;

#include <cstdint>

export module pi.support.hook_bus;

import std;
export import pi.plugin.i_hook_bus;

/** In-process IHookBus. */
export class HookBus : public IHookBus {
public:
    std::uint64_t subscribe(const std::string& event, Handler handler) override;
    void unsubscribe(std::uint64_t id) override;
    bool hasHandlers(const std::string& event) const override;
    HookOutcome emit(const std::string& event, const Json& payload, const Apply& apply = {}) override;

private:
    std::vector<Handler> handlersOf(const std::string& event) const;

    mutable std::mutex m_mutex;
    std::uint64_t m_nextId = 1;
    /** Event to its handlers in subscription order. */
    std::map<std::string, std::vector<std::pair<std::uint64_t, Handler>>> m_handlers;
};

std::uint64_t HookBus::subscribe(const std::string& event, Handler handler) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const std::uint64_t id = m_nextId++;
    m_handlers[event].emplace_back(id, std::move(handler));
    return id;
}

void HookBus::unsubscribe(std::uint64_t id) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& entry : m_handlers) {
        std::erase_if(entry.second, [id](const auto& handler) { return handler.first == id; });
    }
}

bool HookBus::hasHandlers(const std::string& event) const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_handlers.find(event);
    return found != m_handlers.end() && !found->second.empty();
}

std::vector<IHookBus::Handler> HookBus::handlersOf(const std::string& event) const {
    std::vector<Handler> out;
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_handlers.find(event);
    if (found != m_handlers.end()) {
        for (const auto& entry : found->second) {
            out.push_back(entry.second);
        }
    }
    return out;
}

HookOutcome HookBus::emit(const std::string& event, const Json& payload, const Apply& apply) {
    HookOutcome outcome;
    outcome.payload = payload;
    for (const Handler& handler : handlersOf(event)) {
        auto result = handler(event, outcome.payload);
        if (!result) {
            outcome.errors.push_back(result.error());
            continue;
        }
        if (result->is_null()) {
            continue;
        }
        if (apply) {
            apply(outcome.payload, *result);
        }
        outcome.results.push_back(std::move(*result));
    }
    return outcome;
}
