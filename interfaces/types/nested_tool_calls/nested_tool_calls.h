#pragma once

#include <vector>

#include "interfaces/types/nested_tool_call_record/nested_tool_call_record.h"

/** Bounded record of nested calls; `complete` is false when calls were dropped. */
struct NestedToolCalls {
    std::vector<NestedToolCallRecord> calls;
    bool complete = true;
};
