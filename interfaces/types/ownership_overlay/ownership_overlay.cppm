module;

#include <cstdint>

export module pi.types.ownership_overlay;

import std;
export import pi.types.json;

/** Candidate records a commit staged; they override committed records in ownership walks. */
export struct OwnershipOverlay {
    std::map<std::int64_t, Json> tasks;
    /** Owner task of each staged conversation; nullopt when ownerless. */
    std::map<std::int64_t, std::optional<std::int64_t>> edges;
};
