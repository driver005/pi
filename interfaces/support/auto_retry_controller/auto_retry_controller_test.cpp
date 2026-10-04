#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.auto_retry_controller;
import pi.testing.fake_settings_manager;
import pi.testing.recording_session_sink;
import pi.testing.session_harness;

class AutoRetryControllerTest : public testing::Test {
protected:
    AutoRetryControllerTest()
        : m_agent(m_harness.makeAgent()),
          m_settings(Json{{"retry", Json{{"enabled", true}, {"maxRetries", 2}, {"baseDelayMs", 10}}}}),
          m_refresher(*m_agent, m_harness.session()),
          m_omitter(m_harness.session(), m_sink, m_refresher),
          m_retry(m_settings, m_harness.sleeper(), m_omitter, m_sink) {}

    AssistantMessage failedTurn(const std::string& error) {
        AssistantMessage message;
        message.content.push_back(TextContent{"partial", std::nullopt});
        message.stopReason = StopReason::Error;
        message.errorMessage = error;
        message.timestamp = ++m_stamp;
        m_harness.session().appendMessage(message);
        return message;
    }

    SessionHarness m_harness;
    std::unique_ptr<IAgent> m_agent;
    FakeSettingsManager m_settings;
    SessionContextRefresher m_refresher;
    RecordingSessionSink m_sink;
    RecoveryAttemptOmitter m_omitter;
    AutoRetryController m_retry;
    std::int64_t m_stamp = 0;
};

TEST_F(AutoRetryControllerTest, RetryBacksOffExponentiallyAndOmitsTheFailedAttempt) {
    const AssistantMessage first = failedTurn("503 overloaded");
    ASSERT_TRUE(m_retry.prepareRetry(first));
    EXPECT_EQ(m_retry.attempt(), 1);
    const AssistantMessage second = failedTurn("503 overloaded");
    ASSERT_TRUE(m_retry.prepareRetry(second));
    EXPECT_EQ(m_harness.sleeper().delays(), (std::vector<std::int64_t>{10, 20}));
    const auto starts = m_sink.eventsOf(SessionEventType::AutoRetryStart);
    ASSERT_EQ(starts.size(), 2U);
    EXPECT_EQ(starts[0].attempt, 1);
    EXPECT_EQ(starts[0].maxAttempts, 2);
    EXPECT_EQ(starts[0].delayMs, 10);
    EXPECT_EQ(starts[1].delayMs, 20);
    EXPECT_EQ(starts[0].errorMessage, "503 overloaded");
    EXPECT_EQ(m_sink.eventsOf(SessionEventType::EntryAppended).size(), 2U);
}

TEST_F(AutoRetryControllerTest, ExhaustedBudgetKeepsAttemptCountForFinalFailure) {
    ASSERT_TRUE(m_retry.prepareRetry(failedTurn("503 overloaded")));
    ASSERT_TRUE(m_retry.prepareRetry(failedTurn("503 overloaded")));
    const AssistantMessage last = failedTurn("503 still down");
    EXPECT_FALSE(m_retry.prepareRetry(last));
    EXPECT_EQ(m_retry.attempt(), 2);
    m_retry.failed(last);
    EXPECT_EQ(m_retry.attempt(), 0);
    const auto ends = m_sink.eventsOf(SessionEventType::AutoRetryEnd);
    ASSERT_EQ(ends.size(), 1U);
    EXPECT_FALSE(ends[0].success);
    EXPECT_EQ(ends[0].attempt, 2);
    EXPECT_EQ(ends[0].finalError, "503 still down");
}

TEST_F(AutoRetryControllerTest, SuccessEndsTheSequenceOnlyWhenRetrying) {
    m_retry.succeeded();
    EXPECT_TRUE(m_sink.eventsOf(SessionEventType::AutoRetryEnd).empty());
    ASSERT_TRUE(m_retry.prepareRetry(failedTurn("503 overloaded")));
    m_retry.succeeded();
    const auto ends = m_sink.eventsOf(SessionEventType::AutoRetryEnd);
    ASSERT_EQ(ends.size(), 1U);
    EXPECT_TRUE(ends[0].success);
    EXPECT_EQ(m_retry.attempt(), 0);
}

TEST_F(AutoRetryControllerTest, AbortDuringBackoffCancelsTheRetry) {
    m_harness.sleeper().abortOnNextSleep();
    EXPECT_FALSE(m_retry.prepareRetry(failedTurn("503 overloaded")));
    const auto ends = m_sink.eventsOf(SessionEventType::AutoRetryEnd);
    ASSERT_EQ(ends.size(), 1U);
    EXPECT_EQ(ends[0].finalError, "Retry cancelled");
    EXPECT_EQ(m_retry.attempt(), 0);
}

TEST_F(AutoRetryControllerTest, DisabledNeverRetries) {
    m_settings.setGlobalNested("retry", "enabled", false);
    EXPECT_FALSE(m_retry.prepareRetry(failedTurn("503 overloaded")));
    EXPECT_TRUE(m_sink.events().empty());
}

TEST_F(AutoRetryControllerTest, ContextOverflowIsNotRetryable) {
    AssistantMessage overflow;
    overflow.stopReason = StopReason::Error;
    overflow.errorMessage = "prompt is too long: 3 tokens > 2 maximum";
    EXPECT_FALSE(m_retry.isRetryable(overflow, 1000));
    AssistantMessage transient;
    transient.stopReason = StopReason::Error;
    transient.errorMessage = "429 rate limit";
    EXPECT_TRUE(m_retry.isRetryable(transient, 1000));
}

TEST_F(AutoRetryControllerTest, WillRetryAfterAgentEndLooksAtTheLastAssistant) {
    AssistantMessage transient;
    transient.stopReason = StopReason::Error;
    transient.errorMessage = "503 overloaded";
    const std::vector<AgentMessage> messages = {AgentMessage(UserMessage{}), AgentMessage(transient)};
    EXPECT_TRUE(m_retry.willRetryAfterAgentEnd(messages, 1000, false));
    EXPECT_FALSE(m_retry.willRetryAfterAgentEnd(messages, 1000, true));
    EXPECT_FALSE(m_retry.willRetryAfterAgentEnd({AgentMessage(UserMessage{})}, 1000, false));
}
