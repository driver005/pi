module;

#include <cstdint>

export module pi.types.eval_observation;

import std;

/**
 * What one run showed: its identity, the outcome (`scored`, `unscored`, `skipped`, `pending` or `errored`; `score` only when scored)
 * and the operational metrics the harness could measure.
 */
export struct EvalObservation {
    std::string evalSet;
    std::string caseId;
    std::string variant;
    std::string model;
    std::int64_t runNumber = 1;
    std::string outcome = "errored";
    std::optional<double> score;
    std::optional<double> inputTokens;
    std::optional<double> outputTokens;
    std::optional<double> cacheReadTokens;
    std::optional<double> cacheWriteTokens;
    std::optional<double> totalTokens;
    std::optional<double> toolCalls;
    std::optional<double> totalMs;
    std::optional<double> estimatedCostUsd;
};
