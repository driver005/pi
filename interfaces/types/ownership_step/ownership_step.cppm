module;

#include <cstdint>

export module pi.types.ownership_step;

import std;
export import pi.types.task_node;

/** One step of a walk up the ownership tree. */
export struct OwnershipStep {
    /** "task" (with `node`), "conversation", or "unknown" (an owner edge that is not loaded yet). */
    std::string kind = "unknown";
    std::int64_t id = 0;
    TaskNode node;
};
