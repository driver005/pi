#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "interfaces/types/json/json.h"
#include "interfaces/types/nested_tool_calls/nested_tool_calls.h"
#include "interfaces/types/usage/usage.h"
#include "interfaces/types/user_content_block/user_content_block.h"

struct ToolResultMessage {
    std::string toolCallId;
    std::string toolName;
    std::vector<UserContentBlock> content;
    /** Structured details for logs/UI; null means absent. */
    Json details;
    std::optional<Usage> usage;
    std::optional<NestedToolCalls> nestedCalls;
    bool isError = false;
    std::int64_t timestamp = 0;
};
