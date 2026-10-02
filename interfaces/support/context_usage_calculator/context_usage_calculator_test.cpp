#include <gtest/gtest.h>

import std;
import pi.support.context_usage_calculator;

class ContextUsageCalculatorTest : public testing::Test {
protected:
    SessionEntry entry(const std::string& id, const std::string& type) {
        SessionEntry out;
        out.id = id;
        out.type = type;
        return out;
    }

    AgentMessage assistant(std::int64_t tokens) {
        AssistantMessage message;
        message.usage.totalTokens = tokens;
        message.stopReason = StopReason::Stop;
        return message;
    }

    ContextUsageCalculator m_calculator;
};

TEST_F(ContextUsageCalculatorTest, UnknownWindowHasNoUsage) {
    EXPECT_FALSE(m_calculator.calculate(0, {}, {}).has_value());
}

TEST_F(ContextUsageCalculatorTest, ReportsTokensAndPercent) {
    SessionProjection projection;
    projection.entries.push_back({entry("a", "message"), {assistant(2500)}});
    projection.messages = {assistant(2500)};
    const auto usage = m_calculator.calculate(10000, projection, {entry("a", "message")});
    ASSERT_TRUE(usage.has_value());
    EXPECT_EQ(usage->tokens, 2500);
    EXPECT_DOUBLE_EQ(*usage->percent, 25.0);
}

TEST_F(ContextUsageCalculatorTest, UnknownRightAfterCompaction) {
    SessionProjection projection;
    projection.entries.push_back({entry("a", "message"), {assistant(9000)}});
    projection.messages = {assistant(9000)};
    const std::vector<SessionEntry> branch = {entry("a", "message"), entry("c", "compaction")};
    const auto usage = m_calculator.calculate(10000, projection, branch);
    ASSERT_TRUE(usage.has_value());
    EXPECT_FALSE(usage->tokens.has_value());
    EXPECT_FALSE(usage->percent.has_value());
    EXPECT_EQ(usage->contextWindow, 10000);
}

TEST_F(ContextUsageCalculatorTest, KnownOnceAResponseFollowsCompaction) {
    SessionProjection projection;
    projection.entries.push_back({entry("b", "message"), {assistant(1200)}});
    projection.messages = {assistant(1200)};
    const std::vector<SessionEntry> branch = {entry("c", "compaction"), entry("b", "message")};
    const auto usage = m_calculator.calculate(10000, projection, branch);
    EXPECT_EQ(usage->tokens, 1200);
}
