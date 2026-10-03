module;

#include <cstdint>

export module pi.durable.i_registry_reader;

import std;
export import pi.durable.i_registry_snapshot;

/** Read side of a registry consumed by a harness. */
export class IRegistryReader {
public:
    virtual ~IRegistryReader() = default;

    /** An immutable view of the whole current registry. */
    virtual std::shared_ptr<const IRegistrySnapshot> snapshot() const = 0;
    /** Registers a listener called synchronously after every publication; returns its handle. */
    virtual std::int64_t subscribe(std::function<void()> listener) = 0;
    virtual void unsubscribe(std::int64_t handle) = 0;
};
