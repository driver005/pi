module;

#include <cstdint>

export module pi.types.eval_pair_group;

import std;
export import pi.types.eval_observation;

/** The runs of one case, model and repetition across the documentation variants: how many were planned and what was observed. */
export struct EvalPairGroup {
    std::string evalSet;
    std::string caseId;
    std::string model;
    std::int64_t runNumber = 1;
    std::map<std::string, int> expected;
    std::map<std::string, std::vector<EvalObservation>> observations;
};
