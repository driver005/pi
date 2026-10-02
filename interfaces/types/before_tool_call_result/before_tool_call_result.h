#pragma once

#include <optional>
#include <string>

/** Returned by beforeToolCall; block=true replaces execution with an error result. */
struct BeforeToolCallResult {
    bool block = false;
    std::optional<std::string> reason;
    /** Hint to stop after the batch when this call is blocked (all results must agree). */
    bool terminate = false;
};
