module;

#include <cstdint>

export module pi.types.branch_preparation;

import std;
export import pi.types.agent_message;
export import pi.types.file_operations;

/** Messages chosen for a branch summary (chronological), their file operations and size. */
export struct BranchPreparation {
    std::vector<AgentMessage> messages;
    FileOperations fileOps;
    std::int64_t totalTokens = 0;
};
