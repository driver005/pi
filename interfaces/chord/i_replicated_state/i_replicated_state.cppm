module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.chord.i_replicated_state;

import std;
export import pi.types.json;
export import pi.types.replicated_state_snapshot;
export import pi.types.service_context;

/**
 * A replicated state as the service provider sees it: an atomic (value, sequence) snapshot and a
 * stream of operation batches. Each publication carries the ops that turn the previous revision into
 * the new one; sequences count publications from 0 (the initial value) and never skip.
 */
export class IReplicatedState {
public:
    using Listener = std::function<void(const Json& ops, std::int64_t sequence, const ServiceContext& context)>;

    virtual ~IReplicatedState() = default;

    virtual ReplicatedStateSnapshot snapshot() const = 0;
    /** Delivers every later publication in order until unsubscribe(); returns an id. */
    virtual std::uint64_t subscribe(Listener listener) = 0;
    virtual void unsubscribe(std::uint64_t id) = 0;
};
