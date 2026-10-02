module;

#include <cstdint>

export module pi.types.context_usage;

import std;

/** Current context size against the model window; tokens/percent are unknown right after compaction. */
export struct ContextUsage {
    std::optional<std::int64_t> tokens;
    std::int64_t contextWindow = 0;
    std::optional<double> percent;
};
