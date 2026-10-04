module;

#include <cstdint>

export module pi.types.loaded_document;

import std;
export import pi.types.json;

/** One committed document incarnation held in the session's cache. */
export struct LoadedDocument {
    std::string addressId;
    Json record;
    /** Persisted definition version; older while the value is migrated only in memory. */
    std::int64_t storedVersion = 0;
    /** Definition version whose shape `value` has. */
    std::int64_t valueVersion = 0;
    /** Stored deltas after the newest base. */
    std::int64_t deltasSinceBase = 0;
    Json value;
};
