export module pi.types.eval_reading;

import std;
export import pi.types.eval_observation;

/** An observation read from a Vitest report, with the session snapshot (`session.jsonl` text) the run left, when it left one. */
export struct EvalReading {
    EvalObservation observation;
    std::optional<std::string> session;
};
