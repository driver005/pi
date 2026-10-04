export module pi.types.before_compact_outcome;

import std;
export import pi.types.compaction_result;

/** What the `session_before_compact` plugin event decided: cancel, or supply the compaction instead of the model. */
export struct BeforeCompactOutcome {
    bool cancel = false;
    std::optional<CompactionResult> compaction;
};
