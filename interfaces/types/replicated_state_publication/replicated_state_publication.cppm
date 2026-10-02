module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.types.replicated_state_publication;

import std;
export import pi.types.json;
export import pi.types.service_context;

/** One committed revision of a replicated state, queued for delivery to its listeners. */
export struct ReplicatedStatePublication {
    Json ops;
    std::int64_t sequence = 0;
    ServiceContext context;
};
