#include <gtest/gtest.h>

import std;
import pi.support.overflow_detector;

class OverflowDetectorTest : public testing::Test {
protected:
    AssistantMessage error(const std::string& text, const std::string& provider = "p") {
        AssistantMessage message;
        message.stopReason = StopReason::Error;
        message.errorMessage = text;
        message.provider = provider;
        return message;
    }

    OverflowDetector m_detector;
};

TEST_F(OverflowDetectorTest, ProviderErrorTextsAreOverflow) {
    for (const char* text :
         {"prompt is too long: 213462 tokens > 200000 maximum", "413 request_too_large",
          "Your input exceeds the context window of this model",
          "Requested token count exceeds the model's maximum context length of 131072 tokens",
          "The input token count (1196265) exceeds the maximum number of tokens allowed",
          "This model's maximum prompt length is 131072 but the request contains 537812 tokens",
          "prompt token count of 9 exceeds the limit of 8", "Range of input length should be [1, 100]",
          "context_length_exceeded"}) {
        EXPECT_TRUE(m_detector.isContextOverflow(error(text))) << text;
    }
}

TEST_F(OverflowDetectorTest, ThrottlingLookalikesAreNot) {
    EXPECT_FALSE(m_detector.isContextOverflow(error("Throttling error: Too many tokens, please wait")));
    EXPECT_FALSE(m_detector.isContextOverflow(error("rate limit: too many tokens per minute")));
    EXPECT_FALSE(m_detector.isContextOverflow(error("500 internal error")));
}

TEST_F(OverflowDetectorTest, CerebrasBodylessErrorsOnlyForCerebras) {
    EXPECT_TRUE(m_detector.isContextOverflow(error("413 status code (no body)", "cerebras")));
    EXPECT_FALSE(m_detector.isContextOverflow(error("413 status code (no body)", "other")));
}

TEST_F(OverflowDetectorTest, SilentOverflowNeedsContextWindow) {
    AssistantMessage message;
    message.stopReason = StopReason::Stop;
    message.usage.input = 900;
    message.usage.cacheRead = 200;
    EXPECT_FALSE(m_detector.isContextOverflow(message));
    EXPECT_TRUE(m_detector.isContextOverflow(message, 1000));
    EXPECT_FALSE(m_detector.isContextOverflow(message, 2000));
}

TEST_F(OverflowDetectorTest, LengthStopWithNoOutputFillingWindow) {
    AssistantMessage message;
    message.stopReason = StopReason::Length;
    message.usage.input = 995;
    message.usage.output = 0;
    EXPECT_TRUE(m_detector.isContextOverflow(message, 1000));
    message.usage.output = 5;
    EXPECT_FALSE(m_detector.isContextOverflow(message, 1000));
}

TEST_F(OverflowDetectorTest, RecoverableLengthIsBelowDesiredOutput) {
    AssistantMessage message;
    message.stopReason = StopReason::Length;
    message.usage.output = 100;
    EXPECT_TRUE(m_detector.isRecoverableLength(message, 200));
    EXPECT_FALSE(m_detector.isRecoverableLength(message, 100));
    EXPECT_FALSE(m_detector.isRecoverableLength(message, 0));
    message.stopReason = StopReason::Stop;
    EXPECT_FALSE(m_detector.isRecoverableLength(message, 200));
}
