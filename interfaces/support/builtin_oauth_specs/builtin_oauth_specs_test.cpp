#include <gtest/gtest.h>

import std;
import pi.support.builtin_oauth_specs;

TEST(BuiltinOauthSpecsTest, ListsTheSubscriptionProviders) {
    const auto specs = BuiltinOauthSpecs().all();
    std::vector<std::string> ids;
    for (const auto& spec : specs) {
        ids.push_back(spec.providerId);
    }
    EXPECT_EQ(ids, (std::vector<std::string>{"anthropic", "openai-codex", "openai", "xai", "kimi-coding", "openrouter"}));
}

TEST(BuiltinOauthSpecsTest, AnthropicUsesAJsonBodyAndAFiveMinuteMargin) {
    const auto specs = BuiltinOauthSpecs().all();
    EXPECT_TRUE(specs[0].jsonBody);
    EXPECT_EQ(specs[0].clientId, "9d1c250a-e61b-44d9-88ed-5944d1962f5e");
    EXPECT_EQ(specs[0].expiryMarginMs, 300000);
}

TEST(BuiltinOauthSpecsTest, KimiHostCanBeOverridden) {
    EXPECT_EQ(BuiltinOauthSpecs().all()[4].tokenUrl, "https://auth.kimi.com/api/oauth/token");
    EXPECT_EQ(BuiltinOauthSpecs().all("https://kimi.example.com/")[4].tokenUrl, "https://kimi.example.com/api/oauth/token");
}

TEST(BuiltinOauthSpecsTest, ChatGptReadsItsClientIdFromTheCredential) {
    const auto spec = BuiltinOauthSpecs().all()[2];
    EXPECT_TRUE(spec.clientId.empty());
    EXPECT_EQ(spec.requiredScope, "chatgpt.tokens.use.direct");
    EXPECT_EQ(spec.extraParams.at("resource"), "https://api.openai.com/v1");
}
