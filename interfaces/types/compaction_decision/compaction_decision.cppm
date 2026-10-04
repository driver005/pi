export module pi.types.compaction_decision;

import std;

/**
 * What to do after an assistant response. Threshold: compact, do not retry. OverflowNoRetry: the
 * response completed but exceeded the window; compact and keep it. OverflowRetry: drop the failed
 * response, compact, run the turn again. OverflowRecoveryExhausted: a retry already failed once.
 */
export enum class CompactionAction { None, Threshold, OverflowNoRetry, OverflowRetry, OverflowRecoveryExhausted };

export struct CompactionDecision {
    CompactionAction action = CompactionAction::None;
    /** Set for OverflowRecoveryExhausted. */
    std::optional<std::string> errorMessage;
};
