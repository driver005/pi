#include <gtest/gtest.h>

import std;
import pi.support.response_classifier;

class ResponseClassifierTest : public ::testing::Test {
protected:
    Json errorMessage(const std::string& text) {
        return Json::object({{"role", "assistant"}, {"content", Json::array()}, {"stopReason", "error"}, {"errorMessage", text},
                             {"usage", Json::object({{"input", 0}, {"output", 0}, {"cacheRead", 0}, {"cacheWrite", 0}, {"totalTokens", 0},
                                                     {"cost", Json::object({{"input", 0.0}, {"output", 0.0}, {"cacheRead", 0.0}, {"cacheWrite", 0.0}, {"total", 0.0}})}})},
                             {"timestamp", 1}});
    }

    ResponseClassifier m_classifier;
};

TEST_F(ResponseClassifierTest, TransientErrorsAreRetryableAndQuotaOnesAreNot) {
    EXPECT_TRUE(m_classifier.retryable(errorMessage("503 Service Unavailable")));
    EXPECT_TRUE(m_classifier.retryable(errorMessage("rate limit exceeded")));
    EXPECT_FALSE(m_classifier.retryable(errorMessage("insufficient_quota: billing issue 429")));
    EXPECT_FALSE(m_classifier.retryable(errorMessage("invalid request")));
    Json stopped = errorMessage("503");
    stopped["stopReason"] = "stop";
    EXPECT_FALSE(m_classifier.retryable(stopped));
}

TEST_F(ResponseClassifierTest, ContextOverflowIsRecognizedByItsErrorText) {
    EXPECT_TRUE(m_classifier.contextOverflow(errorMessage("prompt is too long: 250000 tokens > 200000 maximum")));
    EXPECT_FALSE(m_classifier.contextOverflow(errorMessage("503 overloaded")));
}

TEST_F(ResponseClassifierTest, DelayDoublesPerAttemptAndIsCapped) {
    ConversationRetryPolicy policy;
    policy.baseDelayMs = 1000;
    policy.maxAgentDelayMs = 5000;
    EXPECT_EQ(m_classifier.retryDelayMs(policy, 1), 1000);
    EXPECT_EQ(m_classifier.retryDelayMs(policy, 2), 2000);
    EXPECT_EQ(m_classifier.retryDelayMs(policy, 3), 4000);
    EXPECT_EQ(m_classifier.retryDelayMs(policy, 4), 5000);
    EXPECT_EQ(m_classifier.retryDelayMs(policy, 100), 5000);
    policy.maxAgentDelayMs.reset();
    EXPECT_EQ(m_classifier.retryDelayMs(policy, 20), 60000);
}
