module;

#include <cstdint>

export module pi.types.boundary;

import std;
export import pi.types.json;

/** What a queue boundary reads before its commit's first table write, and the newest head it has seen so far. */
export struct Boundary {
    std::int64_t conversationId = 0;
    /** The `pi.inbox` draft (`{items: [...]}`) of the commit. */
    Json* inbox = nullptr;
    /** "all" or "one-at-a-time". */
    std::string steeringMode = "one-at-a-time";
    std::string followUpMode = "one-at-a-time";
    /** Start of the active range, the newest head marker's `head`; advanced by heads written in this commit. */
    std::optional<std::int64_t> head;
};
