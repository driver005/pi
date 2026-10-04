module;

#include <cstdint>

export module pi.types.nested_tool_call_record;

import std;
export import pi.types.json;

/** A tool call another tool made while it ran. status: "ok" | "error" | "unfinished". */
export struct NestedToolCallRecord {
    std::string id;
    std::string name;
    /** Omitted (null) when over the size limits; argumentsBytes then gives their size. */
    Json arguments;
    std::optional<std::int64_t> argumentsBytes;
    std::string status = "ok";
    std::optional<std::int64_t> durationMs;
    std::optional<std::string> error;
};
