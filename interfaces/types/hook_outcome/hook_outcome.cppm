export module pi.types.hook_outcome;

import std;
export import pi.types.error;
export import pi.types.json;

/** What emitting a hook event produced. */
export struct HookOutcome {
    /** The payload after every handler's result was applied to it. */
    Json payload;
    /** Every non-null handler result, in subscription order. */
    std::vector<Json> results;
    /** Handlers that failed; their results are skipped. */
    std::vector<Error> errors;
};
