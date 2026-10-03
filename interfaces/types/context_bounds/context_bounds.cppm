module;

#include <cstdint>

export module pi.types.context_bounds;

import std;
export import pi.types.json;

/** The head marker and newest visible entry that fix one committed context range. */
export struct ContextBounds {
    /** The newest applicable head marker entry; null when none. */
    Json head;
    std::int64_t tail = 0;
};
