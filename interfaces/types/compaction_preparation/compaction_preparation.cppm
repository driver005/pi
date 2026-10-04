module;

#include <cstdint>

export module pi.types.compaction_preparation;

import std;
export import pi.types.agent_message;
export import pi.types.compaction_settings;
export import pi.types.file_operations;

/** Everything needed to run one compaction, computed from the session path. */
export struct CompactionPreparation {
    std::string firstKeptEntryId;
    /** Messages that will be summarized and discarded. */
    std::vector<AgentMessage> messagesToSummarize;
    /** Earlier part of a split turn, summarized separately. */
    std::vector<AgentMessage> turnPrefixMessages;
    bool isSplitTurn = false;
    std::int64_t tokensBefore = 0;
    /** Summary of the previous compaction, merged into the new one. */
    std::optional<std::string> previousSummary;
    FileOperations fileOps;
    CompactionSettings settings;
};
