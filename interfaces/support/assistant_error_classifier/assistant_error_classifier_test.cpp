#include <gtest/gtest.h>

import std;
import pi.support.assistant_error_classifier;

class AssistantErrorClassifierTest : public testing::Test {
protected:
    AssistantMessage failed(const std::string& text, StopReason reason = StopReason::Error) {
        AssistantMessage message;
        message.stopReason = reason;
        message.errorMessage = text;
        return message;
    }

    AssistantErrorClassifier m_classifier;
};

TEST_F(AssistantErrorClassifierTest, TransientFailuresAreRetryable) {
    for (const char* text : {"429 rate limit exceeded", "Overloaded", "503 Service Unavailable",
                             "fetch failed", "Anthropic stream ended before message_stop",
                             "request timed out", "socket hang up", "Too Many Requests"}) {
        EXPECT_TRUE(m_classifier.isRetryable(failed(text))) << text;
    }
}

TEST_F(AssistantErrorClassifierTest, QuotaAndBillingAreNot) {
    EXPECT_FALSE(m_classifier.isRetryable(failed("429 insufficient_quota: billing details")));
    EXPECT_FALSE(m_classifier.isRetryable(failed("429 Monthly usage limit reached")));
}

TEST_F(AssistantErrorClassifierTest, DeterministicErrorsAndNonErrorsAreNot) {
    EXPECT_FALSE(m_classifier.isRetryable(failed("400 invalid request body")));
    EXPECT_FALSE(m_classifier.isRetryable(failed("overloaded", StopReason::Aborted)));
    AssistantMessage noText;
    noText.stopReason = StopReason::Error;
    EXPECT_FALSE(m_classifier.isRetryable(noText));
}
