#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.eval_observation_codec;
import pi.support.eval_planner;
import pi.support.eval_report_formatter;
import pi.support.eval_report_summarizer;

class EvalReportSummarizerTest : public testing::Test {
protected:
    std::vector<EvalTask> tasks(const Json& json) {
        std::vector<EvalTask> out;
        for (const Json& entry : json) {
            out.push_back(*m_planner.fromJson(entry));
        }
        return out;
    }

    std::vector<EvalObservation> observations(const Json& json) {
        std::vector<EvalObservation> out;
        for (const Json& entry : json) {
            out.push_back(*m_codec.fromJson(entry));
        }
        return out;
    }

    EvalPlanner m_planner;
    EvalObservationCodec m_codec;
    EvalReportSummarizer m_summarizer;
    EvalReportFormatter m_formatter;
};

TEST_F(EvalReportSummarizerTest, ReproducesTheTypeScriptReportsAndTextForGeneratedRuns) {
    std::ifstream in("src/testing/ts_golden/ts_evals_golden.json");
    ASSERT_TRUE(in.good());
    const Json golden = Json::parse(in);
    ASSERT_FALSE(golden["scenarios"].empty());
    int index = 0;
    for (const Json& scenario : golden["scenarios"]) {
        const Json report = m_summarizer.summarize(scenario["digest"].get<std::string>(), tasks(scenario["tasks"]), observations(scenario["observations"]));
        EXPECT_EQ(report, scenario["report"]) << "scenario " << index << ": " << report.dump() << "\n" << scenario["report"].dump();
        EXPECT_EQ(m_formatter.format(report), scenario["text"].get<std::string>()) << "scenario " << index;
        ++index;
    }
}

TEST_F(EvalReportSummarizerTest, WithholdsThePassRateOfBlockedPairsAndReportsWhy) {
    EvalObservation without;
    without.evalSet = "Set";
    without.caseId = "case";
    without.variant = "without_docs";
    without.model = "p/m";
    without.outcome = "scored";
    without.score = 1;
    EvalObservation with = without;
    with.variant = "with_docs";
    with.outcome = "errored";
    with.score.reset();
    EvalTask a;
    a.evalCase.evalSet = "Set";
    a.evalCase.caseId = "case";
    a.model = "p/m";
    a.variant = "without_docs";
    EvalTask b = a;
    b.variant = "with_docs";
    const Json report = m_summarizer.summarize("d", {a, b}, {without, with});
    ASSERT_EQ(report["comparisons"].size(), 1U);
    EXPECT_TRUE(report["comparisons"][0]["lift"].is_null());
    EXPECT_EQ(report["comparisons"][0]["blockedPairs"], 1);
    EXPECT_EQ(report["blockedPairs"][0]["reasons"][0], "with_docs: errored");
    EXPECT_NE(m_formatter.format(report).find("withheld because pairs are blocked"), std::string::npos);
}

TEST_F(EvalReportSummarizerTest, AnEmptyReportFormatsAsNothing) {
    const Json report = m_summarizer.summarize("d", {}, {});
    EXPECT_TRUE(report["comparisons"].empty());
    EXPECT_EQ(m_formatter.format(report), "");
}
