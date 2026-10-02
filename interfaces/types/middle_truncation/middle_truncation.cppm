module;

#include <cstdint>

export module pi.types.middle_truncation;

import std;

/** Outcome of cutting the middle out of long text; the counts describe the original. */
export struct MiddleTruncation {
    std::string content;
    bool truncated = false;
    /** Characters (not bytes) removed from the middle. */
    std::int64_t removedChars = 0;
    std::int64_t totalBytes = 0;
    std::int64_t totalLines = 0;
};
