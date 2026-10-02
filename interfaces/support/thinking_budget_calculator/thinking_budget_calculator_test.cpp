#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.thinking_budget_calculator;

TEST(ThinkingBudgetCalculatorTest, DefaultBudgets) {
    ThinkingBudgetCalculator calculator;
    EXPECT_EQ(calculator.budgetForLevel(ThinkingLevel::Minimal, Json()), 1024);
    EXPECT_EQ(calculator.budgetForLevel(ThinkingLevel::Low, Json()), 2048);
    EXPECT_EQ(calculator.budgetForLevel(ThinkingLevel::Medium, Json()), 8192);
    EXPECT_EQ(calculator.budgetForLevel(ThinkingLevel::High, Json()), 16384);
    EXPECT_EQ(calculator.budgetForLevel(ThinkingLevel::Max, Json()), 16384);
}

TEST(ThinkingBudgetCalculatorTest, CustomBudgetOverrides) {
    ThinkingBudgetCalculator calculator;
    const Json custom = Json::parse(R"({"high":5000})");
    EXPECT_EQ(calculator.budgetForLevel(ThinkingLevel::XHigh, custom), 5000);
    EXPECT_EQ(calculator.budgetForLevel(ThinkingLevel::Low, custom), 2048);
}

TEST(ThinkingBudgetCalculatorTest, AdjustAddsBudgetToCap) {
    ThinkingBudgetCalculator calculator;
    const auto plan = calculator.adjustMaxTokens(4000, 64000, ThinkingLevel::Medium, Json());
    EXPECT_EQ(plan.maxTokens, 12192);
    EXPECT_EQ(plan.thinkingBudget, 8192);
}

TEST(ThinkingBudgetCalculatorTest, AdjustClampsBudgetWhenCeilingTooSmall) {
    ThinkingBudgetCalculator calculator;
    const auto plan = calculator.adjustMaxTokens(std::nullopt, 4000, ThinkingLevel::High, Json());
    EXPECT_EQ(plan.maxTokens, 4000);
    EXPECT_EQ(plan.thinkingBudget, 4000 - 1024);
}

TEST(ThinkingBudgetCalculatorTest, ClampMaxTokensToContext) {
    ThinkingBudgetCalculator calculator;
    Model model;
    model.contextWindow = 10000;
    TranscriptContext context;
    UserMessage user;
    user.content = std::string(4000, 'x');
    context.messages.emplace_back(user);
    // 10000 - 1000 - 4096 = 4904
    EXPECT_EQ(calculator.clampMaxTokensToContext(model, context, 100000), 4904);
    EXPECT_EQ(calculator.clampMaxTokensToContext(model, context, 100), 100);
    model.contextWindow = 0;
    EXPECT_EQ(calculator.clampMaxTokensToContext(model, context, 100000), 100000);
}
