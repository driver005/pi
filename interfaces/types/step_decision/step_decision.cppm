export module pi.types.step_decision;

import std;

/** What a scheduler step decides for an invocation: continue with the next phase, end, or end by writing `faulted`. */
export struct StepDecision {
    /** "continue", "end" or "fault". */
    std::string kind = "end";
    /** fault: the failure message recorded in the outcome. */
    std::string message;
};
