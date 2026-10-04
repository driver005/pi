#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.eval_planner;

class EvalPlannerTest : public testing::Test {
protected:
    Json discovered() {
        return Json::parse(R"([{"name":"Add model > adds the model","file":"evals/models.docs.eval.ts"}])");
    }

    EvalPlanner m_planner;
};

TEST_F(EvalPlannerTest, DerivesStableCaseIdentityFromOrdinaryVitestNames) {
    const auto cases = m_planner.parseDiscovered(discovered());
    ASSERT_TRUE(cases.has_value());
    ASSERT_EQ(cases->size(), 1U);
    EXPECT_EQ((*cases)[0].fullName, "Add model > adds the model");
    EXPECT_EQ((*cases)[0].evalSet, "Add model");
    EXPECT_EQ((*cases)[0].caseId, "adds the model");
    EXPECT_EQ((*cases)[0].file, "evals/models.docs.eval.ts");
}

TEST_F(EvalPlannerTest, RejectsAmbiguousDuplicateAndMalformedCases) {
    const auto single = m_planner.parseDiscovered(Json::parse(R"([{"name":"adds the model","file":"model.ts"}])"));
    ASSERT_FALSE(single.has_value());
    EXPECT_NE(single.error().message.find("<eval set> > <case>"), std::string::npos);
    EXPECT_FALSE(m_planner.parseDiscovered(Json::parse(R"([{"name":"a > b > c","file":"f"}])")).has_value());
    EXPECT_FALSE(m_planner.parseDiscovered(Json::parse(R"([{"name":" > b","file":"f"}])")).has_value());
    Json twice = discovered();
    twice.push_back(discovered()[0]);
    const auto duplicate = m_planner.parseDiscovered(twice);
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_NE(duplicate.error().message.find("Duplicate eval case identity"), std::string::npos);
    EXPECT_FALSE(m_planner.parseDiscovered(Json::object()).has_value());
    EXPECT_FALSE(m_planner.parseDiscovered(Json::parse(R"([{"name":1,"file":"f"}])")).has_value());
}

TEST_F(EvalPlannerTest, PlansOneTaskPerCaseVariantAndRepetitionAlternatingTheOrder) {
    const auto cases = m_planner.parseDiscovered(discovered());
    const auto tasks = m_planner.plan(*cases, "fixture/model", 2);
    ASSERT_TRUE(tasks.has_value());
    ASSERT_EQ(tasks->size(), 4U);
    const std::vector<std::pair<std::string, std::int64_t>> expected{{"without_docs", 1}, {"with_docs", 1}, {"with_docs", 2}, {"without_docs", 2}};
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ((*tasks)[i].variant, expected[i].first);
        EXPECT_EQ((*tasks)[i].runNumber, expected[i].second);
        EXPECT_EQ((*tasks)[i].model, "fixture/model");
    }
}

TEST_F(EvalPlannerTest, RejectsInvalidModelsAndRepetitions) {
    const auto cases = *m_planner.parseDiscovered(discovered());
    const auto bare = m_planner.plan(cases, "model", 1);
    ASSERT_FALSE(bare.has_value());
    EXPECT_NE(bare.error().message.find("provider and model"), std::string::npos);
    EXPECT_FALSE(m_planner.plan(cases, "/model", 1).has_value());
    EXPECT_FALSE(m_planner.plan(cases, "provider/", 1).has_value());
    const auto none = m_planner.plan(cases, "fixture/model", 0);
    ASSERT_FALSE(none.has_value());
    EXPECT_NE(none.error().message.find("positive integer"), std::string::npos);
}

TEST_F(EvalPlannerTest, TasksRoundTripThroughJson) {
    const auto tasks = *m_planner.plan(*m_planner.parseDiscovered(discovered()), "fixture/model", 1);
    const Json json = m_planner.toJson(tasks[1]);
    EXPECT_EQ(json["variant"], "with_docs");
    EXPECT_EQ(json["runNumber"], 1);
    const auto back = m_planner.fromJson(json);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->evalCase.fullName, tasks[1].evalCase.fullName);
    EXPECT_EQ(back->variant, "with_docs");
    EXPECT_FALSE(m_planner.fromJson(Json::parse(R"({"evalSet":"a"})")).has_value());
}
