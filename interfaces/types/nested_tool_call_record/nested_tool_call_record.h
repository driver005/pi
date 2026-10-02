#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "interfaces/types/json/json.h"

/** A tool call another tool made while it ran. status: "ok" | "error" | "unfinished". */
struct NestedToolCallRecord {
    std::string id;
    std::string name;
    /** Omitted (null) when over the size limits; argumentsBytes then gives their size. */
    Json arguments;
    std::optional<std::int64_t> argumentsBytes;
    std::string status = "ok";
    std::optional<std::int64_t> durationMs;
    std::optional<std::string> error;
};
