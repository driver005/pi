#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.compaction_trigger;

class CompactionTriggerTest : public testing::Test {
protected:
    CompactionTriggerTest() {
        m_settings.reserveTokens = 1000;
    }

    AssistantMessage response(StopReason reason, std::int64_t totalTokens, std::int64_t timestamp = 100) {
        AssistantMessage message;
        message.stopReason = reason;
        message.usage.totalTokens = totalTokens;
        message.usage.input = totalTokens;
        message.timestamp = timestamp;
        return message;
    }

    SessionEntry entry(const std::string& id, const std::string& type, Json body = Json::object()) {
        SessionEntry out;
        out.id = id;
        out.type = type;
        out.timestamp = "1970-01-01T00:00:00.100Z";
        out.body = std::move(body);
        return out;
    }

    CompactionDecision decide(const AssistantMessage& assistant, bool skipAborted = true,
                              bool attempted = false) {
        const std::vector<AgentMessage> messages = {assistant};
        SessionProjection projection;
        projection.entries.push_back({entry("a", "message"), {AgentMessage(assistant)}});
        projection.messages = messages;
        const CompactionCheck check{assistant, skipAborted, m_settings, true, 10000, 4000,
                                    m_assistantId, m_branch, projection, messages, attempted};
        return m_trigger.decide(check);
    }

    CompactionSettings m_settings;
    std::optional<std::string> m_assistantId = std::string("a");
    std::vector<SessionEntry> m_branch = {entry("a", "message")};
    CompactionTrigger m_trigger;
};

TEST_F(CompactionTriggerTest, BelowThresholdDoesNothing) {
    EXPECT_EQ(decide(response(StopReason::Stop, 5000)).action, CompactionAction::None);
}

TEST_F(CompactionTriggerTest, AboveThresholdCompactsWithoutRetry) {
    EXPECT_EQ(decide(response(StopReason::Stop, 9500)).action, CompactionAction::Threshold);
}

TEST_F(CompactionTriggerTest, DisabledAndAbortedAreSkipped) {
    m_settings.enabled = false;
    EXPECT_EQ(decide(response(StopReason::Stop, 9500)).action, CompactionAction::None);
    m_settings.enabled = true;
    EXPECT_EQ(decide(response(StopReason::Aborted, 9500)).action, CompactionAction::None);
    EXPECT_EQ(decide(response(StopReason::Aborted, 9500), false).action, CompactionAction::Threshold);
}

TEST_F(CompactionTriggerTest, OverflowErrorRetriesOnceThenGivesUp) {
    AssistantMessage failed = response(StopReason::Error, 0);
    failed.errorMessage = "prompt is too long: 300000 tokens > 200000 maximum";
    EXPECT_EQ(decide(failed).action, CompactionAction::OverflowRetry);
    const auto exhausted = decide(failed, true, true);
    EXPECT_EQ(exhausted.action, CompactionAction::OverflowRecoveryExhausted);
    EXPECT_EQ(exhausted.errorMessage,
              "Context overflow recovery failed after one compact-and-retry attempt. Try reducing context or switching to a larger-context model.");
}

TEST_F(CompactionTriggerTest, SilentOverflowOnSuccessfulResponseKeepsIt) {
    EXPECT_EQ(decide(response(StopReason::Stop, 12000)).action, CompactionAction::OverflowNoRetry);
}

TEST_F(CompactionTriggerTest, RecoverableLengthStopRetries) {
    AssistantMessage truncated = response(StopReason::Length, 3000);
    truncated.usage.output = 100;
    EXPECT_EQ(decide(truncated).action, CompactionAction::OverflowRetry);
    const auto exhausted = decide(truncated, true, true);
    EXPECT_EQ(exhausted.errorMessage, "Truncated response recovery failed after one compact-and-retry attempt.");
}

TEST_F(CompactionTriggerTest, ResponseFromBeforeLatestCompactionIsIgnored) {
    m_branch = {entry("a", "message"), entry("c", "compaction")};
    m_branch[1].timestamp = "1970-01-01T00:00:00.500Z";
    EXPECT_EQ(decide(response(StopReason::Stop, 9500, 400)).action, CompactionAction::None);
    EXPECT_EQ(decide(response(StopReason::Stop, 9500, 600)).action, CompactionAction::Threshold);
}

TEST_F(CompactionTriggerTest, OmittedResponseIsNotRetriedForExplicitOverflow) {
    AssistantMessage failed = response(StopReason::Error, 0);
    failed.errorMessage = "prompt is too long";
    m_branch = {entry("a", "message"),
                entry("e", "context_edit", Json{{"targetId", "a"}, {"replacement", nullptr}})};
    // The edit invalidates direct usage and omits the response, so no overflow retry is chosen.
    EXPECT_NE(decide(failed).action, CompactionAction::OverflowRetry);
}

TEST_F(CompactionTriggerTest, ErrorResponseUsesEstimateFromLastValidUsage) {
    AssistantMessage failed = response(StopReason::Error, 0);
    failed.errorMessage = "529 overloaded";
    // No valid usage anywhere: the estimate is message-size based and tiny.
    EXPECT_EQ(decide(failed).action, CompactionAction::None);
}
