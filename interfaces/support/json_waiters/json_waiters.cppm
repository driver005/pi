module;

#include <cstdint>

export module pi.support.json_waiters;

import std;
export import pi.support.abort_signal;
export import pi.types.json;
export import pi.types.result;
export import pi.types.waiter_registry;
export import pi.types.waiter_slot;

/**
 * Pending waits by integer key, each settling once: through `resolve` (all waiters of a key get the value),
 * `rejectAll`, or abort of the signal passed to `add`. Registration is separate from blocking so a caller can
 * register while holding a lock and block after releasing it. Copies share one registry.
 */
export class JsonWaiters {
public:
    JsonWaiters() : m_registry(std::make_shared<WaiterRegistry>()) {}

    explicit JsonWaiters(std::shared_ptr<WaiterRegistry> registry) : m_registry(std::move(registry)) {}

    /** Registers a wait under `key`; a signal that aborts settles just this wait with "aborted". */
    std::shared_ptr<WaiterSlot> add(std::int64_t key, const AbortSignal* cancel = nullptr) {
        auto slot = std::make_shared<WaiterSlot>();
        if (cancel != nullptr && cancel->aborted()) {
            settle(slot, std::unexpected(Error{"aborted", "The operation was aborted"}));
            return slot;
        }
        {
            const std::lock_guard<std::mutex> lock(m_registry->mutex);
            m_registry->slots[key].push_back(slot);
        }
        if (cancel != nullptr) {
            const std::weak_ptr<WaiterSlot> weak = slot;
            const std::weak_ptr<WaiterRegistry> registry = m_registry;
            cancel->onAbort([registry, weak] {
                auto live = weak.lock();
                auto shared = registry.lock();
                if (live && shared) {
                    JsonWaiters(shared).cancel(live);
                }
            });
        }
        return slot;
    }

    /** Blocks until the slot settles. */
    Result<Json> await(const std::shared_ptr<WaiterSlot>& slot) const {
        (void)slot->gate.wait();
        const std::lock_guard<std::mutex> lock(slot->mutex);
        return *slot->outcome;
    }

    /** Removes the slot and settles it with "aborted". */
    void cancel(const std::shared_ptr<WaiterSlot>& slot) {
        {
            const std::lock_guard<std::mutex> lock(m_registry->mutex);
            for (auto entry = m_registry->slots.begin(); entry != m_registry->slots.end();) {
                std::erase(entry->second, slot);
                entry = entry->second.empty() ? m_registry->slots.erase(entry) : std::next(entry);
            }
        }
        settle(slot, std::unexpected(Error{"aborted", "The operation was aborted"}));
    }

    std::vector<std::int64_t> keys() const {
        const std::lock_guard<std::mutex> lock(m_registry->mutex);
        std::vector<std::int64_t> keys;
        for (const auto& entry : m_registry->slots) {
            keys.push_back(entry.first);
        }
        return keys;
    }

    void resolve(std::int64_t key, const Json& value) {
        std::vector<std::shared_ptr<WaiterSlot>> slots;
        {
            const std::lock_guard<std::mutex> lock(m_registry->mutex);
            auto found = m_registry->slots.find(key);
            if (found != m_registry->slots.end()) {
                slots = std::move(found->second);
                m_registry->slots.erase(found);
            }
        }
        for (const auto& slot : slots) {
            settle(slot, Result<Json>(value));
        }
    }

    void rejectAll(const Error& error) {
        std::map<std::int64_t, std::vector<std::shared_ptr<WaiterSlot>>> all;
        {
            const std::lock_guard<std::mutex> lock(m_registry->mutex);
            all.swap(m_registry->slots);
        }
        for (const auto& entry : all) {
            for (const auto& slot : entry.second) {
                settle(slot, std::unexpected(error));
            }
        }
    }

private:
    void settle(const std::shared_ptr<WaiterSlot>& slot, Result<Json> outcome) const {
        {
            const std::lock_guard<std::mutex> lock(slot->mutex);
            if (slot->outcome) {
                return;
            }
            slot->outcome = std::move(outcome);
        }
        slot->gate.open();
    }

    std::shared_ptr<WaiterRegistry> m_registry;
};
