export module pi.support.eval_observation_reader;

import std;
export import pi.types.eval_reading;
export import pi.types.eval_task;
export import pi.types.json;

/**
 * Reads what a task's Vitest JSON report says about its one run: the status of the case, the harness run (model, usage, timings,
 * errors, the session snapshot artifact `piSessionJsonl`) and the eval's average score. Anything that does not match the task,
 * such as another case, several cases or another model, makes the run `errored`; a skipped or pending case keeps that outcome; a
 * failed case, errors in the run or an invalid score are `errored` but keep the metrics; no score is `unscored`. The harness data
 * is the `meta` of the report's one assertion. Port of readTaskObservation in packages/evals/src/report.ts.
 */
export class EvalObservationReader {
public:
    EvalReading read(const EvalTask& task, const std::string& reportText) const {
        EvalReading reading;
        reading.observation = identity(task);
        const Json report = Json::parse(reportText, nullptr, false);
        const Json* assertion = onlyAssertion(report);
        if (assertion == nullptr || text(*assertion, "fullName") != task.evalCase.evalSet + " " + task.evalCase.caseId) {
            return reading;
        }
        const std::string status = text(*assertion, "status");
        const std::string byStatus = classify(status);
        if (byStatus == "skipped" || byStatus == "pending") {
            reading.observation.outcome = byStatus;
            return reading;
        }
        const Json meta = assertion->contains("meta") ? (*assertion)["meta"] : Json();
        const Json run = meta.is_object() && meta.contains("harness") && meta["harness"].is_object() && meta["harness"].contains("run") ? meta["harness"]["run"] : Json();
        if (!run.is_object() || !run.contains("usage") || !run["usage"].is_object() || !run.contains("errors") || !run["errors"].is_array()) {
            return reading;
        }
        const Json& usage = run["usage"];
        if (run.contains("artifacts") && run["artifacts"].is_object() && run["artifacts"].contains("piSessionJsonl") && run["artifacts"]["piSessionJsonl"].is_string()) {
            reading.session = run["artifacts"]["piSessionJsonl"].get<std::string>();
        }
        if (!usage.contains("provider") || !usage["provider"].is_string() || usage["provider"].get<std::string>().empty() || !usage.contains("model") || !usage["model"].is_string() ||
            usage["model"].get<std::string>().empty() || usage["provider"].get<std::string>() + "/" + usage["model"].get<std::string>() != task.model) {
            return reading;
        }
        const Json metadata = usage.contains("metadata") && usage["metadata"].is_object() ? usage["metadata"] : Json::object();
        const Json timings = run.contains("timings") && run["timings"].is_object() ? run["timings"] : Json::object();
        EvalObservation& observation = reading.observation;
        if (!metric(usage, "inputTokens", observation.inputTokens) || !metric(usage, "outputTokens", observation.outputTokens) || !metric(metadata, "cacheReadTokens", observation.cacheReadTokens) ||
            !metric(metadata, "cacheWriteTokens", observation.cacheWriteTokens) || !metric(usage, "totalTokens", observation.totalTokens) || !metric(usage, "toolCalls", observation.toolCalls) ||
            !metric(timings, "totalMs", observation.totalMs) || !metric(metadata, "estimatedCostUsd", observation.estimatedCostUsd)) {
            observation = identity(task);
            return reading;
        }
        if (byStatus == "errored" || !run["errors"].empty()) {
            return reading;
        }
        const Json eval = meta.contains("eval") && meta["eval"].is_object() ? meta["eval"] : Json::object();
        if (!eval.contains("avgScore") || eval["avgScore"].is_null()) {
            observation.outcome = "unscored";
            return reading;
        }
        if (!eval["avgScore"].is_number() || eval["avgScore"].get<double>() < 0 || eval["avgScore"].get<double>() > 1) {
            return reading;
        }
        observation.outcome = "scored";
        observation.score = eval["avgScore"].get<double>();
        return reading;
    }

private:
    EvalObservation identity(const EvalTask& task) const {
        EvalObservation observation;
        observation.evalSet = task.evalCase.evalSet;
        observation.caseId = task.evalCase.caseId;
        observation.variant = task.variant;
        observation.model = task.model;
        observation.runNumber = task.runNumber;
        observation.outcome = "errored";
        return observation;
    }

    /** The one assertion of the report, nullptr when there is not exactly one. */
    const Json* onlyAssertion(const Json& report) const {
        if (!report.is_object() || !report.contains("testResults") || !report["testResults"].is_array()) {
            return nullptr;
        }
        const Json* found = nullptr;
        int count = 0;
        for (const Json& file : report["testResults"]) {
            if (!file.is_object() || !file.contains("assertionResults") || !file["assertionResults"].is_array()) {
                continue;
            }
            for (const Json& assertion : file["assertionResults"]) {
                found = &assertion;
                ++count;
            }
        }
        return count == 1 && found->is_object() ? found : nullptr;
    }

    /** "errored", "skipped", "pending", or empty for a case that ran (status passed). */
    std::string classify(const std::string& status) const {
        if (status == "failed") {
            return "errored";
        }
        if (status == "skipped" || status == "todo" || status == "disabled") {
            return "skipped";
        }
        return status == "pending" ? "pending" : "";
    }

    std::string text(const Json& object, const std::string& key) const {
        return object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : "";
    }

    /** Reads an optional finite non-negative number into `out`; false when the field is present but invalid. */
    bool metric(const Json& object, const std::string& key, std::optional<double>& out) const {
        if (!object.contains(key) || object[key].is_null()) {
            return true;
        }
        if (!object[key].is_number() || !std::isfinite(object[key].get<double>()) || object[key].get<double>() < 0) {
            return false;
        }
        out = object[key].get<double>();
        return true;
    }
};
