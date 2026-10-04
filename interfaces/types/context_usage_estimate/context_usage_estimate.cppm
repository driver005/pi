module;

#include <cstdint>

export module pi.types.context_usage_estimate;

import std;

/** Estimated context size; usage is the last reported figure, trailing is estimated after it. */
export struct ContextUsageEstimate {
    std::int64_t tokens = 0;
    std::int64_t usageTokens = 0;
    std::int64_t trailingTokens = 0;
    /** Index of the message whose usage was applied, or -1. */
    int lastUsageIndex = -1;
};
