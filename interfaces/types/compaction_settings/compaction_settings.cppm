module;

#include <cstdint>

export module pi.types.compaction_settings;

import std;

/** When to compact and how much recent context to keep verbatim. */
export struct CompactionSettings {
    bool enabled = true;
    std::int64_t reserveTokens = 16384;
    std::int64_t keepRecentTokens = 20000;
};
