export module pi.types.tool_batch_result;

import std;
export import pi.types.tool_result_message;

/** Tool-result messages of one assistant message plus the batch early-termination verdict. */
export struct ToolBatchResult {
    std::vector<ToolResultMessage> messages;
    bool terminate = false;
};
