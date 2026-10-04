module;

#include <cstdint>

export module pi.types.replicated_state_snapshot;

import std;
export import pi.types.json;

/** A replicated state's value together with the sequence of the publication that produced it. */
export struct ReplicatedStateSnapshot {
    Json value;
    std::int64_t sequence = 0;
};
