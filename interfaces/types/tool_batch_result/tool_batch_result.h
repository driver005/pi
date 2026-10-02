#pragma once

#include <vector>

#include "interfaces/types/tool_result_message/tool_result_message.h"

/** Tool-result messages of one assistant message plus the batch early-termination verdict. */
struct ToolBatchResult {
    std::vector<ToolResultMessage> messages;
    bool terminate = false;
};
