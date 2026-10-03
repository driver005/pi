module;

#include <cstdint>

export module pi.types.task_node;

import std;

/** The immutable ownership fields of a task. */
export struct TaskNode {
    std::int64_t conversationId = 0;
    std::optional<std::int64_t> owner;
    bool background = false;
};
