module;

#include <cstdint>

export module pi.types.conversation_compaction_policy;

/** Automatic compaction thresholds; manual compaction ignores `enabled`. */
export struct ConversationCompactionPolicy {
    /** Threshold and overflow compaction. */
    bool enabled = true;
    /** Room kept free for the answer: generation blocks to compact above `contextWindow - reserveTokens`. */
    std::int64_t reserveTokens = 16384;
    /** Approximate size of the recent context a summary keeps verbatim. */
    std::int64_t keepRecentTokens = 20000;
    /** Background compaction starts this many tokens below the blocking threshold; 0 disables it. */
    std::int64_t backgroundTokens = 32768;
};
