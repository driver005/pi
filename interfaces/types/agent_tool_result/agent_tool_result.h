#pragma once

#include <optional>
#include <vector>

#include "interfaces/types/json/json.h"
#include "interfaces/types/usage/usage.h"
#include "interfaces/types/user_content_block/user_content_block.h"

/** Result of one tool execution (final or partial). */
struct AgentToolResult {
    /** Text or image content sent back to the model. */
    std::vector<UserContentBlock> content;
    /** Arbitrary structured details for logs and UIs; null means none. */
    Json details;
    /** Machine-readable result for programmatic callers; not sent to the model. */
    Json structuredContent;
    std::optional<Usage> usage;
    /** Report a failure without losing details/structuredContent. */
    bool isError = false;
    /** Stop after this tool batch when every finalized result in it sets this. */
    bool terminate = false;
};
