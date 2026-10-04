#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.token_estimator;

TEST(TokenEstimatorTest, CountsFourCharsPerToken) {
    UserMessage user;
    user.content = std::string(40, 'x');
    TokenEstimator estimator;
    const auto estimate = estimator.estimate(std::vector<Message>{user});
    EXPECT_EQ(estimate.tokens, 10);
    EXPECT_EQ(estimate.lastUsageIndex, -1);
}

TEST(TokenEstimatorTest, UsesLastAssistantUsageThenTrailingEstimate) {
    UserMessage first;
    first.content = std::string(400, 'x');
    first.timestamp = 1;
    AssistantMessage assistant;
    assistant.timestamp = 2;
    assistant.usage.totalTokens = 500;
    UserMessage second;
    second.content = std::string(8, 'y');
    second.timestamp = 3;
    TokenEstimator estimator;
    const auto estimate = estimator.estimate(std::vector<Message>{first, assistant, second});
    EXPECT_EQ(estimate.usageTokens, 500);
    EXPECT_EQ(estimate.trailingTokens, 2);
    EXPECT_EQ(estimate.tokens, 502);
    EXPECT_EQ(estimate.lastUsageIndex, 1);
}

TEST(TokenEstimatorTest, IgnoresUsageOlderThanLaterPrefixMessage) {
    AssistantMessage assistant;
    assistant.timestamp = 5;
    assistant.usage.totalTokens = 900;
    UserMessage summary;
    summary.content = std::string(4, 's');
    summary.timestamp = 10;
    TokenEstimator estimator;
    // Assistant came first, then a newer message; the usage still applies (it is the prefix).
    EXPECT_EQ(estimator.estimate(std::vector<Message>{assistant, summary}).usageTokens, 900);
    // A newer message before an older-timestamped assistant means usage does not apply.
    EXPECT_EQ(estimator.estimate(std::vector<Message>{summary, assistant}).usageTokens, 0);
}

TEST(TokenEstimatorTest, ImagesCountAsFixedChars) {
    UserMessage user;
    user.content = std::vector<UserContentBlock>{ImageContent{"d", "image/png"}};
    TokenEstimator estimator;
    EXPECT_EQ(estimator.messageTokens(user), 1200);
}
