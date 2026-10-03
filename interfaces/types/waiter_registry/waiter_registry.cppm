module;

#include <cstdint>

export module pi.types.waiter_registry;

import std;
export import pi.types.waiter_slot;

/** The shared registry of pending waits, held by shared_ptr so abort listeners may outlive their JsonWaiters. */
export struct WaiterRegistry {
    std::mutex mutex;
    std::map<std::int64_t, std::vector<std::shared_ptr<WaiterSlot>>> slots;
};
