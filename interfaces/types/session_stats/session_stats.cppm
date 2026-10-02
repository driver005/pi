module;

#include <cstdint>

export module pi.types.session_stats;

import std;
export import pi.types.context_usage;

/** Message counts and billed usage over every entry of a session, including compacted history. */
export struct SessionStats {
    std::optional<std::string> sessionFile;
    std::string sessionId;
    std::int64_t userMessages = 0;
    std::int64_t assistantMessages = 0;
    std::int64_t toolCalls = 0;
    std::int64_t toolResults = 0;
    std::int64_t totalMessages = 0;
    std::int64_t inputTokens = 0;
    std::int64_t outputTokens = 0;
    std::int64_t cacheReadTokens = 0;
    std::int64_t cacheWriteTokens = 0;
    std::int64_t totalTokens = 0;
    double cost = 0;
    std::optional<ContextUsage> contextUsage;
};
