#include <gtest/gtest.h>

import std;
import pi.support.assistant_call_retrier;
import pi.testing.recording_sleeper;

class AssistantCallRetrierTest : public testing::Test {
protected:
    AssistantMessage failure(const std::string& text) {
        AssistantMessage message;
        message.stopReason = StopReason::Error;
        message.errorMessage = text;
        return message;
    }

    AssistantMessage success() {
        AssistantMessage message;
        message.stopReason = StopReason::Stop;
        return message;
    }

    AssistantRetryPolicy policy(int retries) {
        AssistantRetryPolicy out;
        out.enabled = true;
        out.maxRetries = retries;
        out.baseDelayMs = 100;
        out.maxDelayMs = 250;
        return out;
    }

    AssistantCallRetrier::Produce script(std::vector<AssistantMessage> responses) {
        auto remaining = std::make_shared<std::vector<AssistantMessage>>(std::move(responses));
        auto calls = std::make_shared<int>(0);
        m_calls = calls;
        return [remaining, calls] {
            const int index = std::min<int>((*calls)++, static_cast<int>(remaining->size()) - 1);
            return (*remaining)[static_cast<std::size_t>(index)];
        };
    }

    RecordingSleeper m_sleeper;
    AssistantCallRetrier m_retrier{m_sleeper};
    std::shared_ptr<int> m_calls;
};

TEST_F(AssistantCallRetrierTest, RetriesTransientErrorsWithCappedBackoff) {
    std::vector<std::string> events;
    RetryCallbacks callbacks;
    callbacks.onRetryScheduled = [&](int attempt, int max, std::int64_t delay, const std::string& error) {
        events.push_back("scheduled " + std::to_string(attempt) + "/" + std::to_string(max) + " " +
                         std::to_string(delay) + " " + error);
    };
    callbacks.onRetryAttemptStart = [&] { events.push_back("start"); };
    callbacks.onRetryFinished = [&](bool ok, int attempt, const std::optional<std::string>&) {
        events.push_back("finished " + std::to_string(ok) + " " + std::to_string(attempt));
    };
    const auto result = m_retrier.run(
        script({failure("503 unavailable"), failure("503 unavailable"), failure("503 unavailable"),
                success()}),
        policy(3), nullptr, callbacks);
    EXPECT_EQ(result.stopReason, StopReason::Stop);
    EXPECT_EQ(m_sleeper.delays(), (std::vector<std::int64_t>{100, 200, 250}));
    ASSERT_EQ(events.size(), 7U);
    EXPECT_EQ(events.front(), "scheduled 1/3 100 503 unavailable");
    EXPECT_EQ(events.back(), "finished 1 3");
}

TEST_F(AssistantCallRetrierTest, DeterministicErrorReturnsImmediately) {
    const auto result = m_retrier.run(script({failure("400 bad request")}), policy(3), nullptr, {});
    EXPECT_EQ(result.stopReason, StopReason::Error);
    EXPECT_EQ(*m_calls, 1);
    EXPECT_TRUE(m_sleeper.delays().empty());
}

TEST_F(AssistantCallRetrierTest, ExhaustedBudgetReturnsLastError) {
    std::optional<std::string> finalError;
    RetryCallbacks callbacks;
    callbacks.onRetryFinished = [&](bool, int, const std::optional<std::string>& error) { finalError = error; };
    const auto result = m_retrier.run(script({failure("overloaded")}), policy(2), nullptr, callbacks);
    EXPECT_EQ(result.stopReason, StopReason::Error);
    EXPECT_EQ(*m_calls, 3);
    EXPECT_EQ(finalError, "overloaded");
}

TEST_F(AssistantCallRetrierTest, DisabledPolicyNeverRetries) {
    AssistantRetryPolicy off;
    const auto result = m_retrier.run(script({failure("overloaded")}), off, nullptr, {});
    EXPECT_EQ(result.stopReason, StopReason::Error);
    EXPECT_EQ(*m_calls, 1);
}

TEST_F(AssistantCallRetrierTest, AbortDuringBackoffYieldsAbortedMessage) {
    m_sleeper.abortOnNextSleep();
    const auto signal = std::make_shared<AbortSignal>();
    const auto result = m_retrier.run(script({failure("overloaded"), success()}), policy(3), signal, {});
    EXPECT_EQ(result.stopReason, StopReason::Aborted);
    EXPECT_FALSE(result.errorMessage.has_value());
    EXPECT_EQ(*m_calls, 1);
}

TEST_F(AssistantCallRetrierTest, AbortedResponseIsNotRetried) {
    AssistantMessage aborted;
    aborted.stopReason = StopReason::Aborted;
    const auto result = m_retrier.run(script({aborted}), policy(3), nullptr, {});
    EXPECT_EQ(result.stopReason, StopReason::Aborted);
    EXPECT_EQ(*m_calls, 1);
}
