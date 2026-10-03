module;

#include <cstdint>

export module pi.types.compaction_result;

import std;
export import pi.types.json;
export import pi.types.usage;

/** Outcome of compaction; the session manager adds ids and parent when saving it. */
export struct CompactionResult {
    std::string summary;
    std::string firstKeptEntryId;
    std::int64_t tokensBefore = 0;
    std::optional<std::int64_t> estimatedTokensAfter;
    /** Usage of the model call(s) that produced the summary. */
    std::optional<Usage> usage;
    /** {"readFiles": [...], "modifiedFiles": [...]} or plugin-defined data. */
    Json details;
};
