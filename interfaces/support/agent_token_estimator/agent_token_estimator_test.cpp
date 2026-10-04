#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.agent_token_estimator;

class AgentTokenEstimatorTest : public testing::Test {
protected:
    AgentMessage user(const std::string& text) {
        UserMessage message;
        message.content = text;
        return message;
    }

    AgentMessage assistant(const std::string& text, std::int64_t totalTokens,
                           StopReason reason = StopReason::Stop) {
        AssistantMessage message;
        message.content.push_back(TextContent{text, std::nullopt});
        message.usage.totalTokens = totalTokens;
        message.stopReason = reason;
        return message;
    }

    SessionEntry entry(const std::string& id, const std::string& type) {
        SessionEntry out;
        out.id = id;
        out.type = type;
        return out;
    }

    AgentTokenEstimator m_estimator;
};

TEST_F(AgentTokenEstimatorTest, EstimatesRolesFromCharCounts) {
    EXPECT_EQ(m_estimator.messageTokens(user(std::string(9, 'a'))), 3);
    ToolResultMessage result;
    result.content.push_back(TextContent{std::string(4, 'a'), std::nullopt});
    result.content.push_back(ImageContent{"d", "image/png"});
    EXPECT_EQ(m_estimator.messageTokens(AgentMessage(result)), (4 + 4800 + 3) / 4);
    AssistantMessage call;
    ToolCall tool;
    tool.name = "read";
    tool.arguments = Json{{"path", "a"}};
    call.content.push_back(tool);
    EXPECT_EQ(m_estimator.messageTokens(AgentMessage(call)), (4 + 12 + 3) / 4);
}

TEST_F(AgentTokenEstimatorTest, CustomMessagesUseTheirOwnFields) {
    CustomMessage bash;
    bash.role = "bashExecution";
    bash.data = Json{{"command", "ls"}, {"output", "abcdef"}};
    EXPECT_EQ(m_estimator.messageTokens(AgentMessage(bash)), 2);
    CustomMessage summary;
    summary.role = "compactionSummary";
    summary.data = Json{{"summary", std::string(8, 'x')}};
    EXPECT_EQ(m_estimator.messageTokens(AgentMessage(summary)), 2);
    CustomMessage unknown;
    unknown.role = "mystery";
    EXPECT_EQ(m_estimator.messageTokens(AgentMessage(unknown)), 0);
}

TEST_F(AgentTokenEstimatorTest, LengthCountsUtf16Units) {
    EXPECT_EQ(m_estimator.messageTokens(user("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9")), 1);
    EXPECT_EQ(m_estimator.messageTokens(user("\xF0\x9F\x98\x80\xF0\x9F\x98\x80")), 1);
}

TEST_F(AgentTokenEstimatorTest, EstimateUsesLastValidUsageAndTrailingText) {
    const std::vector<AgentMessage> messages = {
        user("q"), assistant("a", 1000), user(std::string(40, 'x')),
        assistant("failed", 999, StopReason::Error)};
    const auto estimate = m_estimator.estimate(messages);
    EXPECT_EQ(estimate.lastUsageIndex, 1);
    EXPECT_EQ(estimate.usageTokens, 1000);
    EXPECT_EQ(estimate.trailingTokens, 10 + 2);
    EXPECT_EQ(estimate.tokens, 1012);
}

TEST_F(AgentTokenEstimatorTest, EstimateWithoutUsageSumsEverything) {
    const auto estimate = m_estimator.estimate({user(std::string(8, 'a')), user(std::string(4, 'b'))});
    EXPECT_EQ(estimate.lastUsageIndex, -1);
    EXPECT_EQ(estimate.tokens, 3);
}

TEST_F(AgentTokenEstimatorTest, ProjectedEstimateDropsStaleUsageAfterCompaction) {
    SessionProjection projection;
    projection.entries.push_back({entry("a", "message"), {assistant("a", 5000)}});
    projection.entries.push_back({entry("b", "message"), {user(std::string(8, 'x'))}});
    projection.messages = {assistant("a", 5000), user(std::string(8, 'x'))};

    const std::vector<SessionEntry> fresh = {entry("a", "message"), entry("b", "message")};
    EXPECT_EQ(m_estimator.estimateProjected(projection, fresh).tokens, 5000 + 2);

    const std::vector<SessionEntry> stale = {entry("a", "message"), entry("c", "compaction"),
                                             entry("b", "message")};
    EXPECT_EQ(m_estimator.estimateProjected(projection, stale).tokens, 1 + 2);
}

TEST_F(AgentTokenEstimatorTest, ShouldCompactHonorsReserveAndEnabled) {
    CompactionSettings settings;
    settings.reserveTokens = 1000;
    EXPECT_FALSE(m_estimator.shouldCompact(9000, 10000, settings));
    EXPECT_TRUE(m_estimator.shouldCompact(9001, 10000, settings));
    settings.enabled = false;
    EXPECT_FALSE(m_estimator.shouldCompact(100000, 10000, settings));
}

TEST_F(AgentTokenEstimatorTest, LastAssistantUsageReadsSessionEntries) {
    SessionEntry first = entry("1", "message");
    first.body = Json{{"message", Json{{"role", "assistant"},
                                       {"content", Json::array()},
                                       {"api", "x"},
                                       {"provider", "p"},
                                       {"model", "m"},
                                       {"usage", Json{{"input", 1}, {"output", 2}, {"cacheRead", 0},
                                                      {"cacheWrite", 0}, {"totalTokens", 3},
                                                      {"cost", Json{{"input", 0}, {"output", 0},
                                                                    {"cacheRead", 0}, {"cacheWrite", 0},
                                                                    {"total", 0}}}}},
                                       {"stopReason", "stop"},
                                       {"timestamp", 1}}}};
    const auto usage = m_estimator.lastAssistantUsage({first, entry("2", "label")});
    ASSERT_TRUE(usage.has_value());
    EXPECT_EQ(usage->totalTokens, 3);
    EXPECT_FALSE(m_estimator.lastAssistantUsage({entry("2", "label")}).has_value());
}
