module;

#include <cstdint>

export module pi.support.eval_observation_codec;

import std;
export import pi.types.eval_observation;
export import pi.types.json;

/** The JSON of an observation as `observations.jsonl` writes it: the identity, the outcome, `score` when scored and the metrics that were measured. */
export class EvalObservationCodec {
public:
    Json toJson(const EvalObservation& observation) const {
        Json out{{"evalSet", observation.evalSet}, {"caseId", observation.caseId}, {"variant", observation.variant}, {"model", observation.model}, {"runNumber", observation.runNumber}, {"outcome", observation.outcome}};
        if (observation.score) {
            out["score"] = *observation.score;
        }
        for (const auto& [name, metric] : metrics()) {
            if (observation.*metric) {
                out[name] = *(observation.*metric);
            }
        }
        return out;
    }

    /** nullopt when the identity or the outcome is missing or of the wrong type. */
    std::optional<EvalObservation> fromJson(const Json& json) const {
        if (!json.is_object()) {
            return std::nullopt;
        }
        for (const char* key : {"evalSet", "caseId", "variant", "model", "outcome"}) {
            if (!json.contains(key) || !json[key].is_string()) {
                return std::nullopt;
            }
        }
        if (!json.contains("runNumber") || !json["runNumber"].is_number_integer()) {
            return std::nullopt;
        }
        EvalObservation out;
        out.evalSet = json["evalSet"].get<std::string>();
        out.caseId = json["caseId"].get<std::string>();
        out.variant = json["variant"].get<std::string>();
        out.model = json["model"].get<std::string>();
        out.runNumber = json["runNumber"].get<std::int64_t>();
        out.outcome = json["outcome"].get<std::string>();
        if (json.contains("score") && json["score"].is_number()) {
            out.score = json["score"].get<double>();
        }
        for (const auto& [name, metric] : metrics()) {
            if (json.contains(name) && json[name].is_number()) {
                out.*metric = json[name].get<double>();
            }
        }
        return out;
    }

private:
    std::vector<std::pair<std::string, std::optional<double> EvalObservation::*>> metrics() const {
        return {{"inputTokens", &EvalObservation::inputTokens},
                {"outputTokens", &EvalObservation::outputTokens},
                {"cacheReadTokens", &EvalObservation::cacheReadTokens},
                {"cacheWriteTokens", &EvalObservation::cacheWriteTokens},
                {"totalTokens", &EvalObservation::totalTokens},
                {"toolCalls", &EvalObservation::toolCalls},
                {"totalMs", &EvalObservation::totalMs},
                {"estimatedCostUsd", &EvalObservation::estimatedCostUsd}};
    }
};
