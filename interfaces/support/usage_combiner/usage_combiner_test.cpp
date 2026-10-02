#include <gtest/gtest.h>

import std;
import pi.support.usage_combiner;

TEST(UsageCombinerTest, SumsCountsAndCosts) {
    Usage a;
    a.input = 1;
    a.output = 2;
    a.cacheRead = 3;
    a.cacheWrite = 4;
    a.totalTokens = 10;
    a.cost.total = 0.5;
    Usage b = a;
    b.reasoning = 7;
    const Usage sum = UsageCombiner().combine(a, b);
    EXPECT_EQ(sum.input, 2);
    EXPECT_EQ(sum.totalTokens, 20);
    EXPECT_DOUBLE_EQ(sum.cost.total, 1.0);
    EXPECT_EQ(sum.reasoning, 7);
    EXPECT_FALSE(sum.cacheWrite1h.has_value());
}
