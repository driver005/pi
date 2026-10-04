#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.session_stats_calculator;

class SessionStatsCalculatorTest : public testing::Test {
protected:
    Json usage(int input, int output, double cost) {
        return Json{{"input", input}, {"output", output}, {"cacheRead", 1}, {"cacheWrite", 2},
                    {"totalTokens", input + output + 3},
                    {"cost", Json{{"input", 0}, {"output", 0}, {"cacheRead", 0}, {"cacheWrite", 0}, {"total", cost}}}};
    }

    SessionEntry entry(const std::string& type, Json body) {
        SessionEntry out;
        out.type = type;
        out.body = std::move(body);
        return out;
    }

    SessionStatsCalculator m_calculator;
};

TEST_F(SessionStatsCalculatorTest, CountsMessagesToolCallsAndUsage) {
    const std::vector<SessionEntry> entries = {
        entry("message", Json{{"message", Json{{"role", "user"}, {"content", "hi"}}}}),
        entry("message",
              Json{{"message", Json{{"role", "assistant"},
                                    {"content", Json::array({Json{{"type", "text"}, {"text", "x"}},
                                                             Json{{"type", "toolCall"}, {"id", "1"}, {"name", "read"}, {"arguments", Json::object()}}})},
                                    {"usage", usage(10, 5, 0.25)}}}}),
        entry("message", Json{{"message", Json{{"role", "toolResult"}, {"usage", usage(1, 1, 0.5)}}}}),
        entry("usage", Json{{"usage", usage(100, 50, 1.0)}}),
        entry("compaction", Json{{"usage", usage(20, 10, 0.125)}}),
        entry("label", Json::object())};
    const SessionStats stats = m_calculator.calculate(entries);
    EXPECT_EQ(stats.totalMessages, 3);
    EXPECT_EQ(stats.userMessages, 1);
    EXPECT_EQ(stats.assistantMessages, 1);
    EXPECT_EQ(stats.toolResults, 1);
    EXPECT_EQ(stats.toolCalls, 1);
    EXPECT_EQ(stats.inputTokens, 10 + 1 + 100 + 20);
    EXPECT_EQ(stats.outputTokens, 5 + 1 + 50 + 10);
    EXPECT_EQ(stats.cacheReadTokens, 4);
    EXPECT_EQ(stats.cacheWriteTokens, 8);
    EXPECT_EQ(stats.totalTokens, stats.inputTokens + stats.outputTokens + 12);
    EXPECT_DOUBLE_EQ(stats.cost, 1.875);
}

TEST_F(SessionStatsCalculatorTest, EmptySessionIsZero) {
    const SessionStats stats = m_calculator.calculate({});
    EXPECT_EQ(stats.totalMessages, 0);
    EXPECT_EQ(stats.totalTokens, 0);
}
