#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.eval_report_formatter;

class EvalReportFormatterTest : public testing::Test {
protected:
    Json metric(double control, double treatment, double delta) {
        return Json{{"eligiblePairs", 2}, {"controlMean", control}, {"treatmentMean", treatment}, {"meanDelta", delta}};
    }

    Json unavailable() {
        return Json{{"eligiblePairs", 0}, {"controlMean", nullptr}, {"treatmentMean", nullptr}, {"meanDelta", nullptr}};
    }

    Json total(int available, double value) {
        return Json{{"availableRuns", available}, {"total", available == 0 ? Json() : Json(value)}};
    }

    Json report() {
        Json comparison{{"evalSet", "Add model"}, {"totalPairs", 2}, {"eligiblePairs", 2}, {"blockedPairs", 0}, {"controlPassRate", 0.5}, {"treatmentPassRate", 1.0}, {"lift", 0.5}, {"flags", Json::array({"treatment-saturated"})},
                        {"totalTokens", metric(1000, 900, -100)}, {"toolCalls", unavailable()}, {"totalMs", metric(2000.25, 1500, -500.25)}, {"estimatedCostUsd", metric(0.05, 0.04, -0.01)}};
        const auto totals = [this](const std::string& variant) {
            return Json{{"variant", variant}, {"runs", 2}, {"totalTokens", total(2, 1900)}, {"toolCalls", total(0, 0)}, {"totalMs", total(1, 3500)}, {"estimatedCostUsd", total(2, 0.09)}};
        };
        return Json{{"comparisons", Json::array({comparison})}, {"operationalTotals", Json::array({totals("without_docs"), totals("with_docs")})}, {"blockedPairs", Json::array()}};
    }

    EvalReportFormatter m_formatter;
};

TEST_F(EvalReportFormatterTest, RendersTheComparisonTotalsAndUnavailableMetrics) {
    const std::string expected =
        "Documentation Eval Comparisons\n"
        "  Add model\n"
        "         Pairs  2/2 eligible\n"
        "     Pass rate  +50.0 pp (with 100.0%, without 50.0%)\n"
        "         Flags  treatment-saturated\n"
        "        Tokens  -100.0 (with 900.0, without 1000.0, 2 pairs)\n"
        "         Tools  unavailable\n"
        "       Latency  -500.3ms (with 1500.0ms, without 2000.3ms, 2 pairs)\n"
        "     Est. cost  -$0.0100 (with $0.0400, without $0.0500, 2 pairs)\n"
        "  Operational totals\n"
        "    without_docs: 2 runs, 1900 tokens, unavailable (0/2 measured), 3.50s (1/2 measured), $0.0900 cost\n"
        "    with_docs: 2 runs, 1900 tokens, unavailable (0/2 measured), 3.50s (1/2 measured), $0.0900 cost";
    EXPECT_EQ(m_formatter.format(report()), expected);
}

TEST_F(EvalReportFormatterTest, WithheldPassRatesAndBlockedPairsAreExplained) {
    Json blocked = report();
    blocked["comparisons"][0]["lift"] = nullptr;
    blocked["comparisons"][0]["blockedPairs"] = 1;
    blocked["comparisons"][0]["flags"] = Json::array();
    blocked["blockedPairs"] = Json::array({Json{{"evalSet", "Add model"}, {"caseId", "adds"}, {"model", "p/m"}, {"runNumber", 2}, {"reasons", Json::array({"with_docs: errored", "without_docs: skipped"})}}});
    const std::string text = m_formatter.format(blocked);
    EXPECT_NE(text.find("     Pass rate  withheld because pairs are blocked\n"), std::string::npos);
    EXPECT_EQ(text.find("Flags"), std::string::npos);
    EXPECT_NE(text.find("  Blocked pairs\n    Add model/adds/p/m/run-2: with_docs: errored; without_docs: skipped"), std::string::npos);
    blocked["comparisons"][0]["blockedPairs"] = 0;
    EXPECT_NE(m_formatter.format(blocked).find("     Pass rate  unavailable\n"), std::string::npos);
}

TEST_F(EvalReportFormatterTest, AReportWithoutComparisonsIsEmpty) {
    EXPECT_EQ(m_formatter.format(Json{{"comparisons", Json::array()}}), "");
}
