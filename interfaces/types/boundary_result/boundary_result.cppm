module;

#include <cstdint>

export module pi.types.boundary_result;

import std;

/** The user items a boundary placed, in id order, and whether a `head: "self"` write (a reset) was placed. */
export struct BoundaryResult {
    std::vector<std::int64_t> users;
    bool reset = false;
};
