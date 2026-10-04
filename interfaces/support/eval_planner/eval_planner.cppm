export module pi.support.eval_planner;

import std;
export import pi.types.discovered_eval_case;
export import pi.types.eval_task;
export import pi.types.json;
export import pi.types.result;

/**
 * Plans documentation-lift evals: turns Vitest's discovered test names (`<eval set> > <case>`) into cases with a stable identity, and
 * cases into one task per case, documentation variant and repetition. The variant order alternates by repetition (without first on
 * odd runs, with first on even ones) to reduce order bias. Port of packages/evals/src/plan.ts.
 */
export class EvalPlanner {
public:
    /** `discovered` is Vitest's list: `[{name, file}]`. Errors: not an array, an invalid entry, a name that is not `<set> > <case>`, a duplicate identity. */
    Result<std::vector<DiscoveredEvalCase>> parseDiscovered(const Json& discovered) const {
        if (!discovered.is_array()) {
            return std::unexpected(Error{"invalid_plan", "Discovered eval cases must be an array."});
        }
        std::set<std::pair<std::string, std::string>> identities;
        std::vector<DiscoveredEvalCase> cases;
        for (const Json& item : discovered) {
            if (!item.is_object() || !item.contains("name") || !item["name"].is_string() || !item.contains("file") || !item["file"].is_string()) {
                return std::unexpected(Error{"invalid_plan", "Discovered eval case is invalid."});
            }
            const std::string name = item["name"].get<std::string>();
            const std::vector<std::string> parts = split(name);
            if (parts.size() != 2 || blank(parts[0]) || blank(parts[1])) {
                return std::unexpected(Error{"invalid_plan", "Documentation eval must use \"<eval set> > <case>\": " + name});
            }
            if (!identities.emplace(parts[0], parts[1]).second) {
                return std::unexpected(Error{"invalid_plan", "Duplicate eval case identity: " + name});
            }
            cases.push_back(DiscoveredEvalCase{item["file"].get<std::string>(), name, parts[0], parts[1]});
        }
        return cases;
    }

    /** `model` is `<provider>/<model>`; `runsPerVariant` at least 1. */
    Result<std::vector<EvalTask>> plan(const std::vector<DiscoveredEvalCase>& cases, const std::string& model, std::int64_t runsPerVariant) const {
        if (model.find('/') == std::string::npos || model.starts_with("/") || model.ends_with("/")) {
            return std::unexpected(Error{"invalid_plan", "Model identity must contain a provider and model."});
        }
        if (runsPerVariant < 1) {
            return std::unexpected(Error{"invalid_plan", "Runs per variant must be a positive integer."});
        }
        std::vector<EvalTask> tasks;
        for (const DiscoveredEvalCase& evalCase : cases) {
            for (std::int64_t run = 1; run <= runsPerVariant; ++run) {
                const std::array<std::string, 2> variants = run % 2 == 1 ? std::array<std::string, 2>{"without_docs", "with_docs"} : std::array<std::string, 2>{"with_docs", "without_docs"};
                for (const std::string& variant : variants) {
                    tasks.push_back(EvalTask{evalCase, variant, model, run});
                }
            }
        }
        return tasks;
    }

    /** The task as the artifact files write it: `{file, fullName, evalSet, caseId, variant, model, runNumber}`. */
    Json toJson(const EvalTask& task) const {
        return Json{{"file", task.evalCase.file}, {"fullName", task.evalCase.fullName}, {"evalSet", task.evalCase.evalSet}, {"caseId", task.evalCase.caseId}, {"variant", task.variant}, {"model", task.model}, {"runNumber", task.runNumber}};
    }

    /** The inverse of toJson; nullopt when a field is missing or of the wrong type. */
    std::optional<EvalTask> fromJson(const Json& json) const {
        if (!json.is_object()) {
            return std::nullopt;
        }
        for (const char* key : {"evalSet", "caseId", "variant", "model"}) {
            if (!json.contains(key) || !json[key].is_string()) {
                return std::nullopt;
            }
        }
        if (!json.contains("runNumber") || !json["runNumber"].is_number_integer()) {
            return std::nullopt;
        }
        EvalTask task;
        task.evalCase.file = json.value("file", std::string());
        task.evalCase.fullName = json.value("fullName", std::string());
        task.evalCase.evalSet = json["evalSet"].get<std::string>();
        task.evalCase.caseId = json["caseId"].get<std::string>();
        task.variant = json["variant"].get<std::string>();
        task.model = json["model"].get<std::string>();
        task.runNumber = json["runNumber"].get<std::int64_t>();
        return task;
    }

private:
    /** Splits at " > " like String.split. */
    std::vector<std::string> split(const std::string& text) const {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (true) {
            const std::size_t at = text.find(" > ", start);
            if (at == std::string::npos) {
                parts.push_back(text.substr(start));
                return parts;
            }
            parts.push_back(text.substr(start, at - start));
            start = at + 3;
        }
    }

    bool blank(const std::string& text) const {
        return text.find_first_not_of(" \t\r\n") == std::string::npos;
    }
};
