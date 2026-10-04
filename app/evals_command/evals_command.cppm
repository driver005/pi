module;

#include <cstdint>

export module pi.evals_command;

import std;
export import pi.coding_services;
export import pi.types.command_line;
import pi.platform.i_file_system;
import pi.support.eval_observation_codec;
import pi.support.eval_observation_reader;
import pi.support.eval_planner;
import pi.support.eval_report_formatter;
import pi.support.eval_report_summarizer;
import pi.support.path_resolver;

/**
 * The analysis side of the documentation evals (packages/evals): `pi evals plan <discovered.json> --model <provider/model> [--runs n]`
 * expands Vitest's discovered cases (`[{name, file}]`) into one task per case, variant and repetition and prints them as JSON;
 * `pi evals observe <task.json> <vitest-report.json>` reads what one run's Vitest JSON report says and prints the observation;
 * `pi evals report <artifact-dir>` pairs the `expected-runs.json` and `observations.jsonl` an eval run left (or any directory with
 * those files), writes `report.json` and `report.txt` next to them and prints the comparison, exiting 1 when pairs are blocked.
 * The Docker orchestration and the Vitest harness stay in TypeScript. Returns the process exit code.
 */
export class EvalsCommand {
public:
    EvalsCommand(CodingServices& services, std::ostream& out, std::ostream& err)
        : m_services(services),
          m_out(out),
          m_err(err) {}

    int run(const CommandLine& line) {
        const std::string& action = line.arguments[0];
        if (action == "plan") {
            return plan(line);
        }
        if (action == "observe") {
            return observe(line);
        }
        return report(line);
    }

private:
    int plan(const CommandLine& line) {
        if (!line.options.startup.model) {
            return fail("pi evals plan needs --model <provider/model>");
        }
        const auto text = read(line, line.arguments[1]);
        if (!text) {
            return fail(text.error().message);
        }
        const auto cases = m_planner.parseDiscovered(Json::parse(*text, nullptr, false));
        if (!cases) {
            return fail(cases.error().message);
        }
        const auto tasks = m_planner.plan(*cases, *line.options.startup.model, line.evalRuns);
        if (!tasks) {
            return fail(tasks.error().message);
        }
        Json out = Json::array();
        for (const EvalTask& task : *tasks) {
            out.push_back(m_planner.toJson(task));
        }
        m_out << out.dump(2) << "\n";
        return 0;
    }

    int observe(const CommandLine& line) {
        const auto taskText = read(line, line.arguments[1]);
        const auto reportText = read(line, line.arguments[2]);
        if (!taskText || !reportText) {
            return fail(!taskText ? taskText.error().message : reportText.error().message);
        }
        const auto task = m_planner.fromJson(Json::parse(*taskText, nullptr, false));
        if (!task) {
            return fail("The task file must hold {evalSet, caseId, variant, model, runNumber}");
        }
        m_out << m_codec.toJson(m_reader.read(*task, *reportText).observation).dump() << "\n";
        return 0;
    }

    int report(const CommandLine& line) {
        const std::string directory = resolve(line, line.arguments[1]);
        const auto expectedText = m_services.platform().files().readFile(directory + "/expected-runs.json");
        const auto observationText = m_services.platform().files().readFile(directory + "/observations.jsonl");
        if (!expectedText || !observationText) {
            return fail("An eval run directory needs expected-runs.json and observations.jsonl: " + directory);
        }
        std::vector<EvalTask> tasks;
        for (const Json& entry : Json::parse(*expectedText, nullptr, false)) {
            const auto task = m_planner.fromJson(entry);
            if (!task) {
                return fail("expected-runs.json has an invalid task");
            }
            tasks.push_back(*task);
        }
        std::vector<EvalObservation> observations;
        std::istringstream lines(*observationText);
        for (std::string text; std::getline(lines, text);) {
            if (text.empty()) {
                continue;
            }
            const auto observation = m_codec.fromJson(Json::parse(text, nullptr, false));
            if (!observation) {
                return fail("observations.jsonl has an invalid line");
            }
            observations.push_back(*observation);
        }
        std::string digest;
        if (const auto protocol = m_services.platform().files().readFile(directory + "/protocol.json")) {
            const Json parsed = Json::parse(*protocol, nullptr, false);
            digest = parsed.is_object() ? parsed.value("protocolDigest", std::string()) : std::string();
        }
        const Json summary = m_summarizer.summarize(digest, tasks, observations);
        const std::string text = m_formatter.format(summary);
        IFileSystem& files = m_services.platform().files();
        if (auto written = files.writeFile(directory + "/report.json", summary.dump(2) + "\n"); !written) {
            return fail(written.error().message);
        }
        if (auto written = files.writeFile(directory + "/report.txt", text + "\n"); !written) {
            return fail(written.error().message);
        }
        m_out << text << "\n";
        return summary["blockedPairs"].empty() ? 0 : 1;
    }

    Result<std::string> read(const CommandLine& line, const std::string& path) {
        return m_services.platform().files().readFile(resolve(line, path));
    }

    std::string resolve(const CommandLine& line, const std::string& path) {
        return PathResolver(m_services.platform().files().homeDirectory()).resolveToCwd(path, line.options.cwd);
    }

    int fail(const std::string& message) {
        m_err << "pi: " << message << "\n";
        return 1;
    }

    CodingServices& m_services;
    std::ostream& m_out;
    std::ostream& m_err;
    EvalPlanner m_planner;
    EvalObservationCodec m_codec;
    EvalObservationReader m_reader;
    EvalReportSummarizer m_summarizer;
    EvalReportFormatter m_formatter;
};
