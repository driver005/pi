#pragma once

#include <optional>
#include <vector>

#include "interfaces/types/json/json.h"
#include "interfaces/types/usage/usage.h"
#include "interfaces/types/user_content_block/user_content_block.h"

/**
 * Partial override returned by afterToolCall; unset fields keep the executed values.
 * Setting `content` without `structuredContent` drops the old structured content.
 */
struct AfterToolCallResult {
    std::optional<std::vector<UserContentBlock>> content;
    Json details;
    Json structuredContent;
    std::optional<bool> isError;
    std::optional<Usage> usage;
    std::optional<bool> terminate;
};
