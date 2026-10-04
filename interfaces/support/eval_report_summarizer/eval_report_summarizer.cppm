module;

#include <cstdint>

export module pi.support.eval_report_summarizer;

import std;
export import pi.types.eval_observation;
export import pi.types.eval_pair_group;
export import pi.types.eval_task;
export import pi.types.json;
import pi.support.js_number_format;

/**
 * Compares the documentation variants of a planned eval run: pairs the `without_docs` and `with_docs` runs of every case,
 * model and repetition, withholds the pass rate of an eval set whose pairs are blocked (a run missing, unscored, errored,
 * skipped or pending), and reports the lift in pass rate with paired means of tokens, tool calls, latency and cost, the
 * flags `no-lift`, `negative-delta`, `control-saturated`, `treatment-saturated` and `flaky`, and the operational totals of each
 * variant. A run passes with a score of 1. Port of summarizeEvalObservations in packages/evals/src/report.ts; the result is the
 * JSON of its report (schemaVersion 3). Names sort bytewise where TypeScript uses locale order.
 */
export class EvalReportSummarizer {
public:
    using Metric = std::optional<double> EvalObservation::*;

    Json summarize(const std::string& protocolDigest, const std::vector<EvalTask>& expected, const std::vector<EvalObservation>& observations) const {
        Json blockedPairs = Json::array();
        std::map<std::string, std::vector<std::pair<const EvalObservation*, const EvalObservation*>>> pairsByEvalSet;
        std::map<std::string, int> totalsByEvalSet;
        const std::vector<EvalPairGroup> groups = groupPairs(expected, observations);
        for (const EvalPairGroup& group : groups) {
            ++totalsByEvalSet[group.evalSet];
            std::vector<std::string> reasons = blockReasons(group);
            if (!reasons.empty()) {
                blockedPairs.push_back(Json{{"evalSet", group.evalSet}, {"caseId", group.caseId}, {"model", group.model}, {"runNumber", group.runNumber}, {"reasons", reasons}});
                continue;
            }
            pairsByEvalSet[group.evalSet].emplace_back(&group.observations.at("without_docs")[0], &group.observations.at("with_docs")[0]);
        }
        Json comparisons = Json::array();
        for (const auto& [evalSet, totalPairs] : totalsByEvalSet) {
            comparisons.push_back(compare(evalSet, totalPairs, pairsByEvalSet[evalSet]));
        }
        return Json{{"schemaVersion", 3},
                    {"protocolDigest", protocolDigest},
                    {"control", "without_docs"},
                    {"treatment", "with_docs"},
                    {"comparisons", comparisons},
                    {"blockedPairs", blockedPairs},
                    {"operationalTotals", Json::array({variantTotals(observations, "without_docs"), variantTotals(observations, "with_docs")})}};
    }

private:
    using Pair = std::pair<const EvalObservation*, const EvalObservation*>;

    /** Groups ordered by eval set, case, model and repetition. */
    std::vector<EvalPairGroup> groupPairs(const std::vector<EvalTask>& expected, const std::vector<EvalObservation>& observations) const {
        std::map<std::tuple<std::string, std::string, std::string, std::int64_t>, EvalPairGroup> groups;
        const auto groupFor = [&groups](const std::string& evalSet, const std::string& caseId, const std::string& model, std::int64_t run) -> EvalPairGroup& {
            EvalPairGroup& group = groups[{evalSet, caseId, model, run}];
            group.evalSet = evalSet;
            group.caseId = caseId;
            group.model = model;
            group.runNumber = run;
            return group;
        };
        for (const EvalTask& task : expected) {
            ++groupFor(task.evalCase.evalSet, task.evalCase.caseId, task.model, task.runNumber).expected[task.variant];
        }
        for (const EvalObservation& observation : observations) {
            groupFor(observation.evalSet, observation.caseId, observation.model, observation.runNumber).observations[observation.variant].push_back(observation);
        }
        std::vector<EvalPairGroup> ordered;
        for (auto& entry : groups) {
            ordered.push_back(std::move(entry.second));
        }
        return ordered;
    }

    std::vector<std::string> blockReasons(const EvalPairGroup& group) const {
        std::vector<std::string> reasons;
        for (const char* variant : {"without_docs", "with_docs"}) {
            const auto planned = group.expected.find(variant);
            const int expected = planned == group.expected.end() ? 0 : planned->second;
            const auto seen = group.observations.find(variant);
            const std::size_t observed = seen == group.observations.end() ? 0 : seen->second.size();
            if (expected != 1) {
                reasons.push_back(std::string(variant) + ": design expected 1 run, found " + std::to_string(expected));
            }
            if (observed != static_cast<std::size_t>(expected)) {
                reasons.push_back(std::string(variant) + ": expected " + std::to_string(expected) + " observation" + (expected == 1 ? "" : "s") + ", found " + std::to_string(observed));
            }
            if (expected == 1 && observed == 1 && seen->second[0].outcome != "scored") {
                reasons.push_back(std::string(variant) + ": " + seen->second[0].outcome);
            }
        }
        return reasons;
    }

    Json compare(const std::string& evalSet, int totalPairs, const std::vector<Pair>& pairs) const {
        const int blocked = totalPairs - static_cast<int>(pairs.size());
        const bool headline = blocked == 0 && !pairs.empty();
        std::optional<double> controlRate;
        std::optional<double> treatmentRate;
        if (headline) {
            controlRate = static_cast<double>(std::ranges::count_if(pairs, [](const Pair& pair) { return pair.first->score.value_or(0) >= 1; })) / static_cast<double>(pairs.size());
            treatmentRate = static_cast<double>(std::ranges::count_if(pairs, [](const Pair& pair) { return pair.second->score.value_or(0) >= 1; })) / static_cast<double>(pairs.size());
        }
        return Json{{"evalSet", evalSet},
                    {"totalPairs", totalPairs},
                    {"eligiblePairs", pairs.size()},
                    {"blockedPairs", blocked},
                    {"controlPassRate", number(controlRate)},
                    {"treatmentPassRate", number(treatmentRate)},
                    {"lift", controlRate && treatmentRate ? Json(m_format.roundTo15(*treatmentRate - *controlRate)) : Json()},
                    {"flags", flags(pairs, controlRate, treatmentRate)},
                    {"totalTokens", pairedMetric(pairs, &EvalObservation::totalTokens)},
                    {"toolCalls", pairedMetric(pairs, &EvalObservation::toolCalls)},
                    {"totalMs", pairedMetric(pairs, &EvalObservation::totalMs)},
                    {"estimatedCostUsd", pairedMetric(pairs, &EvalObservation::estimatedCostUsd)}};
    }

    Json flags(const std::vector<Pair>& pairs, const std::optional<double>& controlRate, const std::optional<double>& treatmentRate) const {
        Json out = Json::array();
        if (controlRate && treatmentRate) {
            if (*controlRate == *treatmentRate) {
                out.push_back("no-lift");
            }
            if (*treatmentRate < *controlRate) {
                out.push_back("negative-delta");
            }
            if (*controlRate == 1) {
                out.push_back("control-saturated");
            }
            if (*treatmentRate == 1) {
                out.push_back("treatment-saturated");
            }
        }
        std::map<std::pair<std::string, std::string>, std::set<bool>> outcomes;
        for (const Pair& pair : pairs) {
            outcomes[{pair.first->caseId, pair.first->variant}].insert(pair.first->score.value_or(0) >= 1);
            outcomes[{pair.second->caseId, pair.second->variant}].insert(pair.second->score.value_or(0) >= 1);
        }
        if (std::ranges::any_of(outcomes, [](const auto& entry) { return entry.second.size() > 1; })) {
            out.push_back("flaky");
        }
        return out;
    }

    Json pairedMetric(const std::vector<Pair>& pairs, Metric metric) const {
        std::vector<double> control;
        std::vector<double> treatment;
        for (const Pair& pair : pairs) {
            const auto& controlValue = (*pair.first).*metric;
            const auto& treatmentValue = (*pair.second).*metric;
            if (controlValue && treatmentValue) {
                control.push_back(*controlValue);
                treatment.push_back(*treatmentValue);
            }
        }
        const std::optional<double> controlMean = mean(control);
        const std::optional<double> treatmentMean = mean(treatment);
        return Json{{"eligiblePairs", control.size()},
                    {"controlMean", number(controlMean)},
                    {"treatmentMean", number(treatmentMean)},
                    {"meanDelta", controlMean && treatmentMean ? Json(m_format.roundTo15(*treatmentMean - *controlMean)) : Json()}};
    }

    Json variantTotals(const std::vector<EvalObservation>& observations, const std::string& variant) const {
        std::vector<const EvalObservation*> runs;
        for (const EvalObservation& observation : observations) {
            if (observation.variant == variant) {
                runs.push_back(&observation);
            }
        }
        return Json{{"variant", variant},
                    {"runs", runs.size()},
                    {"inputTokens", operationalTotal(runs, &EvalObservation::inputTokens)},
                    {"outputTokens", operationalTotal(runs, &EvalObservation::outputTokens)},
                    {"cacheReadTokens", operationalTotal(runs, &EvalObservation::cacheReadTokens)},
                    {"cacheWriteTokens", operationalTotal(runs, &EvalObservation::cacheWriteTokens)},
                    {"totalTokens", operationalTotal(runs, &EvalObservation::totalTokens)},
                    {"toolCalls", operationalTotal(runs, &EvalObservation::toolCalls)},
                    {"totalMs", operationalTotal(runs, &EvalObservation::totalMs)},
                    {"estimatedCostUsd", operationalTotal(runs, &EvalObservation::estimatedCostUsd)}};
    }

    Json operationalTotal(const std::vector<const EvalObservation*>& runs, Metric metric) const {
        std::size_t available = 0;
        double total = 0;
        for (const EvalObservation* run : runs) {
            if ((*run).*metric) {
                ++available;
                total += *((*run).*metric);
            }
        }
        return Json{{"availableRuns", available}, {"total", available == 0 ? Json() : Json(total)}};
    }

    std::optional<double> mean(const std::vector<double>& values) const {
        if (values.empty()) {
            return std::nullopt;
        }
        double sum = 0;
        for (const double value : values) {
            sum += value;
        }
        return sum / static_cast<double>(values.size());
    }

    Json number(const std::optional<double>& value) const {
        return value ? Json(*value) : Json();
    }

    JsNumberFormat m_format;
};
