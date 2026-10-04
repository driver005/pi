#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.support.builtin_oauth_specs;
import pi.support.oauth_token_mapper;
import pi.testing.fixed_clock;

class OauthTokenMapperTest : public testing::Test {
protected:
    OauthTokenMapper mapperFor(const std::string& providerId) {
        for (const auto& candidate : BuiltinOauthSpecs().all()) {
            if (candidate.providerId == providerId) {
                return OauthTokenMapper(candidate, m_clock, m_base64);
            }
        }
        return OauthTokenMapper(OauthRefreshSpec{}, m_clock, m_base64);
    }

    Base64Codec m_base64;
    FixedClock m_clock{1'000'000};
};

TEST_F(OauthTokenMapperTest, TokenRequestsAreFormsOrJsonAsTheSpecSays) {
    const HttpRequest form = mapperFor("openai-codex").tokenRequest({{"grant_type", "authorization_code"}, {"code", "a b"}}, nullptr);
    EXPECT_EQ(form.url, "https://auth.openai.com/oauth/token");
    EXPECT_EQ(form.body, "code=a%20b&grant_type=authorization_code");
    const HttpRequest json = mapperFor("anthropic").tokenRequest({{"grant_type", "authorization_code"}, {"code", "c"}}, nullptr);
    EXPECT_EQ(Json::parse(json.body)["code"], "c");
    const HttpRequest extra = mapperFor("openai").tokenRequest({{"grant_type", "authorization_code"}}, nullptr);
    EXPECT_NE(extra.body.find("resource=https%3A%2F%2Fapi.openai.com%2Fv1"), std::string::npos);
}

TEST_F(OauthTokenMapperTest, ResponsesBecomeCredentialsWithTheSpecsMarginAndExtras) {
    const auto anthropic = mapperFor("anthropic").credentialFrom(Json{{"access_token", "a"}, {"refresh_token", "r"}, {"expires_in", 3600}}, Credential{}, "client");
    ASSERT_TRUE(anthropic.has_value());
    EXPECT_EQ(anthropic->access, "a");
    EXPECT_DOUBLE_EQ(anthropic->expires, 1'000'000 + 3'600'000 - 300'000);
    EXPECT_FALSE(anthropic->extra.contains("clientId"));

    const auto chatgpt = mapperFor("openai").credentialFrom(Json{{"access_token", "a"}, {"refresh_token", "r"}, {"expires_in", 60}, {"scope", "openid chatgpt.tokens.use.direct"}}, Credential{}, "issued");
    ASSERT_TRUE(chatgpt.has_value());
    EXPECT_EQ(chatgpt->extra["clientId"], "issued");
    EXPECT_EQ(chatgpt->extra["scopes"].size(), 2U);
    EXPECT_FALSE(mapperFor("openai").credentialFrom(Json{{"access_token", "a"}, {"refresh_token", "r"}, {"expires_in", 60}, {"scope", "openid"}}, Credential{}, "issued").has_value());
    EXPECT_FALSE(mapperFor("anthropic").credentialFrom(Json{{"access_token", "a"}}, Credential{}, "c").has_value());
}
