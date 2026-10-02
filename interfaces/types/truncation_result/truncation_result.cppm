module;

#include <cstdint>

export module pi.types.truncation_result;

import std;

/** Outcome of head/tail truncation of tool output; counts describe original vs kept content. */
export struct TruncationResult {
    std::string content;
    bool truncated = false;
    /** "lines", "bytes" or empty when not truncated. */
    std::string truncatedBy;
    std::int64_t totalLines = 0;
    std::int64_t totalBytes = 0;
    std::int64_t outputLines = 0;
    std::int64_t outputBytes = 0;
    bool lastLinePartial = false;
    bool firstLineExceedsLimit = false;
    std::int64_t maxLines = 0;
    std::int64_t maxBytes = 0;
};
