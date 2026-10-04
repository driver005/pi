#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.evals_command;
import pi.types.json;

class EvalsCommandTest : public testing::Test {
protected:
    EvalsCommandTest() {
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/evals_command_" + testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir + "/agent");
        std::filesystem::create_directories(m_dir + "/run");
        m_services = std::make_unique<CodingServices>(m_dir + "/agent", m_dir + "/agent/catalog", true);
    }

    void write(const std::string& name, const std::string& content) {
        std::ofstream(m_dir + "/" + name) << content;
    }

    std::string read(const std::string& name) {
        std::ifstream in(m_dir + "/" + name);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    int run(const std::vector<std::string>& arguments, std::optional<std::string> model = std::nullopt, std::int64_t runs = 1) {
        CommandLine line;
        line.command = "evals";
        line.arguments = arguments;
        line.options.startup.model = std::move(model);
        line.evalRuns = runs;
        line.options.cwd = m_dir;
        line.options.agentDir = m_dir + "/agent";
        m_out.str("");
        m_err.str("");
        EvalsCommand command(*m_services, m_out, m_err);
        return command.run(line);
    }

    std::string m_dir;
    std::unique_ptr<CodingServices> m_services;
    std::ostringstream m_out;
    std::ostringstream m_err;
};

TEST_F(EvalsCommandTest, PlansTasksForDiscoveredCases) {
    write("found.json", R"([{"name":"Add model > adds the model","file":"evals/models.docs.eval.ts"}])");
    ASSERT_EQ(run({"plan", "found.json"}, "fixture/model", 2), 0) << m_err.str();
    const Json tasks = Json::parse(m_out.str());
    ASSERT_EQ(tasks.size(), 4U);
    EXPECT_EQ(tasks[0]["variant"], "without_docs");
    EXPECT_EQ(tasks[2]["variant"], "with_docs");
    EXPECT_EQ(tasks[2]["runNumber"], 2);
    EXPECT_EQ(tasks[0]["evalSet"], "Add model");

    EXPECT_EQ(run({"plan", "found.json"}), 1);
    EXPECT_NE(m_err.str().find("--model"), std::string::npos);
    EXPECT_EQ(run({"plan", "found.json"}, "model"), 1);
    EXPECT_NE(m_err.str().find("provider and model"), std::string::npos);
    EXPECT_EQ(run({"plan", "missing.json"}, "a/b"), 1);
}

TEST_F(EvalsCommandTest, ObservesOneRunFromItsVitestReport) {
    write("task.json", R"({"evalSet":"Example","caseId":"case","variant":"with_docs","model":"fixture/model","runNumber":1})");
    const Json meta{{"eval", Json{{"avgScore", 1}}}, {"harness", Json{{"run", Json{{"usage", Json{{"provider", "fixture"}, {"model", "model"}, {"totalTokens", 42}}}, {"errors", Json::array()}}}}}};
    write("vitest.json", Json{{"testResults", Json::array({Json{{"assertionResults", Json::array({Json{{"fullName", "Example case"}, {"status", "passed"}, {"meta", meta}}})}}})}}.dump());
    ASSERT_EQ(run({"observe", "task.json", "vitest.json"}), 0) << m_err.str();
    const Json observation = Json::parse(m_out.str());
    EXPECT_EQ(observation["outcome"], "scored");
    EXPECT_EQ(observation["score"], 1);
    EXPECT_EQ(observation["totalTokens"], 42);
    EXPECT_EQ(observation["variant"], "with_docs");
}

TEST_F(EvalsCommandTest, ReportsTheComparisonOfAnEvalRunDirectory) {
    const auto task = [](const std::string& variant) { return Json{{"evalSet", "Set"}, {"caseId", "case"}, {"variant", variant}, {"model", "p/m"}, {"runNumber", 1}}; };
    write("run/expected-runs.json", Json::array({task("without_docs"), task("with_docs")}).dump());
    write("run/protocol.json", R"({"protocolDigest":"abc"})");
    const auto observation = [](const std::string& variant, double score) { return Json{{"evalSet", "Set"}, {"caseId", "case"}, {"variant", variant}, {"model", "p/m"}, {"runNumber", 1}, {"outcome", "scored"}, {"score", score}, {"totalTokens", 100}}.dump(); };
    write("run/observations.jsonl", observation("without_docs", 0) + "\n" + observation("with_docs", 1) + "\n");
    ASSERT_EQ(run({"report", "run"}), 0) << m_err.str();
    EXPECT_NE(m_out.str().find("Pass rate  +100.0 pp (with 100.0%, without 0.0%)"), std::string::npos) << m_out.str();
    const Json report = Json::parse(read("run/report.json"));
    EXPECT_EQ(report["protocolDigest"], "abc");
    EXPECT_EQ(report["comparisons"][0]["lift"], 1);
    EXPECT_NE(read("run/report.txt").find("Documentation Eval Comparisons"), std::string::npos);

    write("run/observations.jsonl", observation("without_docs", 0) + "\n");
    EXPECT_EQ(run({"report", "run"}), 1) << "a blocked pair fails the run";
    EXPECT_NE(m_out.str().find("with_docs: expected 1 observation, found 0"), std::string::npos);
    EXPECT_EQ(run({"report", "nowhere"}), 1);
}
