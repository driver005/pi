#include <gtest/gtest.h>

import std;
import pi.support.cost_calculator;

TEST(CostCalculatorTest, BasePrices) {
    Model model;
    model.cost.input = 3;
    model.cost.output = 15;
    model.cost.cacheRead = 0.3;
    model.cost.cacheWrite = 3.75;
    Usage usage;
    usage.input = 1000000;
    usage.output = 1000000;
    usage.cacheRead = 1000000;
    usage.cacheWrite = 1000000;
    CostCalculator calculator;
    const auto cost = calculator.calculate(model, usage);
    EXPECT_DOUBLE_EQ(cost.input, 3);
    EXPECT_DOUBLE_EQ(cost.output, 15);
    EXPECT_DOUBLE_EQ(cost.cacheRead, 0.3);
    EXPECT_DOUBLE_EQ(cost.cacheWrite, 3.75);
    EXPECT_DOUBLE_EQ(cost.total, 3 + 15 + 0.3 + 3.75);
}

TEST(CostCalculatorTest, LongCacheWritesCostDoubleInput) {
    Model model;
    model.cost.input = 3;
    model.cost.cacheWrite = 3.75;
    Usage usage;
    usage.cacheWrite = 2000000;
    usage.cacheWrite1h = 1000000;
    CostCalculator calculator;
    EXPECT_DOUBLE_EQ(calculator.calculate(model, usage).cacheWrite, 3.75 + 6);
}

TEST(CostCalculatorTest, HighestMatchingTierWins) {
    Model model;
    model.cost.input = 1;
    model.cost.tiers = {{100, 2, 0, 0, 0}, {1000, 4, 0, 0, 0}};
    Usage usage;
    usage.input = 2000;
    CostCalculator calculator;
    EXPECT_DOUBLE_EQ(calculator.calculate(model, usage).input, 4.0 * 2000 / 1e6);
    usage.input = 500;
    EXPECT_DOUBLE_EQ(calculator.calculate(model, usage).input, 2.0 * 500 / 1e6);
    usage.input = 50;
    EXPECT_DOUBLE_EQ(calculator.calculate(model, usage).input, 1.0 * 50 / 1e6);
}
