#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.responses_compat_resolver;

class ResponsesCompatResolverTest : public testing::Test {
protected:
    ResponsesCompatResolver m_resolver;
};

TEST_F(ResponsesCompatResolverTest, DefaultsForPlainOpenAi) {
    Model model;
    model.provider = "openai";
    model.baseUrl = "https://api.openai.com/v1";
    const ResponsesCompat compat = m_resolver.resolve(model);
    EXPECT_TRUE(compat.supportsDeveloperRole);
    EXPECT_FALSE(compat.supportsMidConvoSystemMessages);
    EXPECT_EQ(compat.sessionAffinityFormat, "openai");
    EXPECT_TRUE(compat.supportsLongCacheRetention);
    EXPECT_TRUE(compat.supportsMaxOutputTokens);
}

TEST_F(ResponsesCompatResolverTest, OpenRouterUsesItsAffinityFormat) {
    Model model;
    model.provider = "other";
    model.baseUrl = "https://openrouter.ai/api/v1";
    EXPECT_EQ(m_resolver.resolve(model).sessionAffinityFormat, "openrouter");
}

TEST_F(ResponsesCompatResolverTest, ExplicitOverridesWin) {
    Model model;
    model.provider = "openrouter";
    model.compat = Json::parse(R"({"supportsDeveloperRole":false,"supportsMaxOutputTokens":false,
                                   "sessionAffinityFormat":"openai","supportsStrictMode":true})");
    const ResponsesCompat compat = m_resolver.resolve(model);
    EXPECT_FALSE(compat.supportsDeveloperRole);
    EXPECT_FALSE(compat.supportsMaxOutputTokens);
    EXPECT_TRUE(compat.supportsStrictMode);
    EXPECT_EQ(compat.sessionAffinityFormat, "openai");
}
