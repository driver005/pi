export module pi.types.after_tool_call_result;

import std;
export import pi.types.json;
export import pi.types.usage;
export import pi.types.user_content_block;

/**
 * Partial override returned by afterToolCall; unset fields keep the executed values.
 * Setting `content` without `structuredContent` drops the old structured content.
 */
export struct AfterToolCallResult {
    std::optional<std::vector<UserContentBlock>> content;
    Json details;
    Json structuredContent;
    std::optional<bool> isError;
    std::optional<Usage> usage;
    std::optional<bool> terminate;
};
