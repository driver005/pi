export module pi.types.before_tool_call_result;

import std;

/** Returned by beforeToolCall; block=true replaces execution with an error result. */
export struct BeforeToolCallResult {
    bool block = false;
    std::optional<std::string> reason;
    /** Hint to stop after the batch when this call is blocked (all results must agree). */
    bool terminate = false;
};
