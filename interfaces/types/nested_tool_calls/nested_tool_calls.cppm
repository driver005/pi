export module pi.types.nested_tool_calls;

import std;
export import pi.types.nested_tool_call_record;

/** Bounded record of nested calls; `complete` is false when calls were dropped. */
export struct NestedToolCalls {
    std::vector<NestedToolCallRecord> calls;
    bool complete = true;
};
