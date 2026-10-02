module;
#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.types.tool_result_message;

import std;
export import pi.types.json;
export import pi.types.nested_tool_calls;
export import pi.types.usage;
export import pi.types.user_content_block;

export struct ToolResultMessage {
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
