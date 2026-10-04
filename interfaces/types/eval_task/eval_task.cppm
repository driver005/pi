module;

#include <cstdint>

export module pi.types.eval_task;

import std;
export import pi.types.discovered_eval_case;

/** One run to execute: a case in one documentation variant (`without_docs` or `with_docs`) with one model, in one repetition. */
export struct EvalTask {
    DiscoveredEvalCase evalCase;
    std::string variant;
    std::string model;
    std::int64_t runNumber = 1;
};
