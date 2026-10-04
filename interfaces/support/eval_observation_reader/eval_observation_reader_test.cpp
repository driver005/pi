#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.eval_observation_reader;

class EvalObservationReaderTest : public testing::Test {
protected:
    EvalObservationReaderTest() {
        m_task.evalCase.file = "evals/example.docs.eval.ts";
        m_task.evalCase.fullName = "Example workflow > handles the case";
        m_task.evalCase.evalSet = "Example workflow";
        m_task.evalCase.caseId = "handles the case";
        m_task.variant = "without_docs";
        m_task.model = "fixture/model";
        m_task.runNumber = 1;
    }

    Json meta(const Json& overrides = Json::object()) {
        Json out{{"eval", Json{{"avgScore", 0.5}, {"scores", Json::array()}}},
                 {"harness",
                  Json{{"name", "without_docs"},
                       {"run",
                        Json{{"usage", Json{{"provider", "fixture"}, {"model", "model"}, {"inputTokens", 10}, {"outputTokens", 5}, {"totalTokens", 15}, {"toolCalls", 1}, {"metadata", Json{{"cacheReadTokens", 2}, {"cacheWriteTokens", 3}, {"estimatedCostUsd", 0.01}}}}},
                             {"timings", Json{{"totalMs", 1234}}},
                             {"artifacts", Json{{"runId", "run-1"}, {"piSessionJsonl", "{\"type\":\"session\"}\n"}}},
                             {"errors", Json::array()}}}}}};
        for (const auto& entry : overrides.items()) {
            out[entry.key()] = entry.value();
        }
        return out;
    }

    std::string report(const std::string& status, const Json& metaJson, const std::string& fullName = "Example workflow handles the case") {
        Json assertion{{"fullName", fullName}, {"status", status}, {"title", "x"}, {"meta", metaJson}};
        return Json{{"testResults", Json::array({Json{{"name", "/repo/packages/evals/evals/example.docs.eval.ts"}, {"assertionResults", Json::array({assertion})}}})}}.dump();
    }

    EvalReading read(const std::string& text) {
        return m_reader.read(m_task, text);
    }

    EvalTask m_task;
    EvalObservationReader m_reader;
};

TEST_F(EvalObservationReaderTest, ReadsAScoredRunWithItsMetricsAndSession) {
    const EvalReading reading = read(report("passed", meta()));
    const EvalObservation& o = reading.observation;
    EXPECT_EQ(o.outcome, "scored");
    EXPECT_EQ(o.score, 0.5);
    EXPECT_EQ(o.evalSet, "Example workflow");
    EXPECT_EQ(o.variant, "without_docs");
    EXPECT_EQ(o.runNumber, 1);
    EXPECT_EQ(o.inputTokens, 10);
    EXPECT_EQ(o.outputTokens, 5);
    EXPECT_EQ(o.cacheReadTokens, 2);
    EXPECT_EQ(o.cacheWriteTokens, 3);
    EXPECT_EQ(o.totalTokens, 15);
    EXPECT_EQ(o.toolCalls, 1);
    EXPECT_EQ(o.totalMs, 1234);
    EXPECT_EQ(o.estimatedCostUsd, 0.01);
    EXPECT_EQ(reading.session, "{\"type\":\"session\"}\n");
}

TEST_F(EvalObservationReaderTest, SkippedAndPendingCasesKeepTheirOutcomeWithoutAHarnessRun) {
    EXPECT_EQ(read(report("skipped", Json::object())).observation.outcome, "skipped");
    EXPECT_EQ(read(report("todo", Json::object())).observation.outcome, "skipped");
    EXPECT_EQ(read(report("disabled", Json::object())).observation.outcome, "skipped");
    EXPECT_EQ(read(report("pending", Json::object())).observation.outcome, "pending");
}

TEST_F(EvalObservationReaderTest, AFailedCaseOrRunErrorsKeepTheMetrics) {
    const EvalObservation failed = read(report("failed", meta())).observation;
    EXPECT_EQ(failed.outcome, "errored");
    EXPECT_EQ(failed.totalTokens, 15);
    EXPECT_FALSE(failed.score.has_value());

    Json withErrors = meta();
    withErrors["harness"]["run"]["errors"] = Json::array({Json{{"message", "Prompt verification failed"}}});
    const EvalObservation errored = read(report("passed", withErrors)).observation;
    EXPECT_EQ(errored.outcome, "errored");
    EXPECT_EQ(errored.toolCalls, 1);
}

TEST_F(EvalObservationReaderTest, MissingOrInvalidScoresAreUnscoredOrErrored) {
    Json noScore = meta();
    noScore["eval"]["avgScore"] = nullptr;
    EXPECT_EQ(read(report("passed", noScore)).observation.outcome, "unscored");
    Json missing = meta();
    missing["eval"] = Json::object();
    EXPECT_EQ(read(report("passed", missing)).observation.outcome, "unscored");
    Json high = meta();
    high["eval"]["avgScore"] = 1.5;
    const EvalObservation invalid = read(report("passed", high)).observation;
    EXPECT_EQ(invalid.outcome, "errored");
    EXPECT_EQ(invalid.totalTokens, 15) << "the metrics stay";
    Json text = meta();
    text["eval"]["avgScore"] = "high";
    EXPECT_EQ(read(report("passed", text)).observation.outcome, "errored");
}

TEST_F(EvalObservationReaderTest, ARunOfAnotherModelCaseOrShapeIsErrored) {
    Json other = meta();
    other["harness"]["run"]["usage"]["model"] = "different";
    const EvalReading wrongModel = read(report("passed", other));
    EXPECT_EQ(wrongModel.observation.outcome, "errored");
    EXPECT_FALSE(wrongModel.observation.totalTokens.has_value());
    EXPECT_EQ(wrongModel.session, "{\"type\":\"session\"}\n") << "the snapshot is kept for inspection";

    EXPECT_EQ(read(report("passed", meta(), "Other case")).observation.outcome, "errored");
    EXPECT_EQ(read(report("passed", Json::object())).observation.outcome, "errored") << "no harness run";
    EXPECT_EQ(read("not json").observation.outcome, "errored");
    EXPECT_EQ(read(R"({"testResults":[]})").observation.outcome, "errored");

    Json twice = Json::parse(report("passed", meta()));
    twice["testResults"][0]["assertionResults"].push_back(twice["testResults"][0]["assertionResults"][0]);
    EXPECT_EQ(read(twice.dump()).observation.outcome, "errored") << "exactly one case per report";
}

TEST_F(EvalObservationReaderTest, AnInvalidMetricMakesTheRunErroredWithoutMetrics) {
    Json negative = meta();
    negative["harness"]["run"]["usage"]["toolCalls"] = -1;
    const EvalObservation observation = read(report("passed", negative)).observation;
    EXPECT_EQ(observation.outcome, "errored");
    EXPECT_FALSE(observation.inputTokens.has_value());
    EXPECT_FALSE(observation.totalTokens.has_value());
}
