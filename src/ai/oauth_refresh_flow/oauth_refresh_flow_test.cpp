#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.oauth_refresh_flow;
import pi.base.base64_codec;
import pi.support.builtin_oauth_specs;
import pi.testing.fixed_clock;
import pi.testing.scripted_http_client;

class OauthRefreshFlowTest : public testing::Test {
protected:
    OauthRefreshSpec spec(const std::string& providerId) {
        for (const auto& candidate : BuiltinOauthSpecs().all()) {
            if (candidate.providerId == providerId) {
                return candidate;
            }
        }
        return {};
    }

    Credential oauth(const std::string& refresh) {
        Credential credential;
        credential.type = CredentialType::OAuth;
        credential.access = "old";
        credential.refresh = refresh;
        return credential;
    }

    HttpResponse reply(const Json& body, int status = 200) {
        HttpResponse response;
        response.status = status;
        response.body = body.dump();
        return response;
    }

    Result<Credential> refresh(const OauthRefreshSpec& spec, const Credential& credential) {
        OauthRefreshFlow flow(spec, m_http, m_clock, m_base64);
        return flow.refresh(credential, nullptr);
    }

    Base64Codec m_base64;
    ScriptedHttpClient m_http;
    FixedClock m_clock{1'000'000};
};

TEST_F(OauthRefreshFlowTest, AnthropicRefreshesWithAJsonBody) {
    m_http.enqueue(reply(Json{{"access_token", "new-access"}, {"refresh_token", "new-refresh"}, {"expires_in", 3600}}));
    const auto result = refresh(spec("anthropic"), oauth("old-refresh"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->access, "new-access");
    EXPECT_EQ(result->refresh, "new-refresh");
    EXPECT_DOUBLE_EQ(result->expires, 1'000'000 + 3600 * 1000 - 5 * 60 * 1000);
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://platform.claude.com/v1/oauth/token");
    const Json sent = Json::parse(request.body);
    EXPECT_EQ(sent["grant_type"], "refresh_token");
    EXPECT_EQ(sent["client_id"], "9d1c250a-e61b-44d9-88ed-5944d1962f5e");
    EXPECT_EQ(sent["refresh_token"], "old-refresh");
    EXPECT_EQ(sent.size(), 3U);
}

TEST_F(OauthRefreshFlowTest, CodexUsesFormBodyAndStoresTheAccountId) {
    const std::string token = "h." + m_base64.encodeUrl(R"({"https://api.openai.com/auth":{"chatgpt_account_id":"acct-9"}})") + ".s";
    m_http.enqueue(reply(Json{{"access_token", token}, {"refresh_token", "r2"}, {"expires_in", 100}}));
    const auto result = refresh(spec("openai-codex"), oauth("r1"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->extra["accountId"], "acct-9");
    EXPECT_DOUBLE_EQ(result->expires, 1'000'000 + 100 * 1000);
    const HttpRequest request = m_http.requests()[0];
    EXPECT_NE(request.body.find("grant_type=refresh_token"), std::string::npos);
    EXPECT_NE(request.body.find("client_id=app_EMoamEEZ73f0CkXaXp7hrann"), std::string::npos);
    EXPECT_NE(request.body.find("refresh_token=r1"), std::string::npos);
}

TEST_F(OauthRefreshFlowTest, CodexRejectsTokensWithoutAnAccountId) {
    m_http.enqueue(reply(Json{{"access_token", "plain"}, {"refresh_token", "r2"}, {"expires_in", 100}}));
    const auto result = refresh(spec("openai-codex"), oauth("r1"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Failed to extract accountId from token");
}

TEST_F(OauthRefreshFlowTest, ChatGptNeedsTheIssuedClientIdAndTheDirectScope) {
    Credential stored = oauth("r1");
    const auto missing = refresh(spec("openai"), stored);
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().message.find("reconnect ChatGPT"), std::string::npos);
    EXPECT_EQ(m_http.calls(), 0);

    stored.extra["clientId"] = "issued-client";
    m_http.enqueue(reply(Json{{"access_token", "a"}, {"refresh_token", "r2"}, {"expires_in", 600},
                              {"scope", "openid chatgpt.tokens.use.direct offline_access"}}));
    const auto ok = refresh(spec("openai"), stored);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->extra["clientId"], "issued-client");
    EXPECT_EQ(ok->extra["scopes"], (Json::array({"openid", "chatgpt.tokens.use.direct", "offline_access"})));
    EXPECT_DOUBLE_EQ(ok->expires, 1'000'000 + 600 * 1000 - 3 * 60 * 1000);
    EXPECT_NE(m_http.requests()[0].body.find("resource=https%3A%2F%2Fapi.openai.com%2Fv1"), std::string::npos);

    m_http.enqueue(reply(Json{{"access_token", "a"}, {"refresh_token", "r2"}, {"expires_in", 600}, {"scope", "openid"}}));
    EXPECT_FALSE(refresh(spec("openai"), stored).has_value());
}

TEST_F(OauthRefreshFlowTest, XaiKeepsTheRefreshTokenWhenItIsNotRotated) {
    m_http.enqueue(reply(Json{{"access_token", "xa"}}));
    const auto result = refresh(spec("xai"), oauth("keep-me"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->refresh, "keep-me");
    EXPECT_DOUBLE_EQ(result->expires, 1'000'000 + 3600 * 1000 - 5 * 60 * 1000);
}

TEST_F(OauthRefreshFlowTest, MissingRefreshTokensAreErrorsWhereRotationIsRequired) {
    m_http.enqueue(reply(Json{{"access_token", "a"}, {"expires_in", 10}}));
    const auto result = refresh(spec("anthropic"), oauth("r"));
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("invalid refresh_token"), std::string::npos);
}

TEST_F(OauthRefreshFlowTest, HttpAndTransportFailuresAreReported) {
    m_http.enqueue(reply(Json{{"error", "invalid_grant"}}, 400));
    const auto rejected = refresh(spec("anthropic"), oauth("r"));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_NE(rejected.error().message.find("(400)"), std::string::npos);
    EXPECT_NE(rejected.error().message.find("invalid_grant"), std::string::npos);
    EXPECT_FALSE(refresh(spec("anthropic"), oauth("r")).has_value());
}

TEST_F(OauthRefreshFlowTest, OpenRouterKeysPassThroughUnchanged) {
    Credential key = oauth("");
    key.access = "sk-or-123";
    const auto result = refresh(spec("openrouter"), key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->access, "sk-or-123");
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(OauthRefreshFlowTest, ToAuthUsesAnApiKeyOrABearerHeader) {
    OauthRefreshFlow anthropic(spec("anthropic"), m_http, m_clock, m_base64);
    Credential credential = oauth("r");
    credential.access = "tok";
    EXPECT_EQ(anthropic.toAuth(credential).apiKey, "tok");
    OauthRefreshFlow kimi(spec("kimi-coding"), m_http, m_clock, m_base64);
    const ModelAuth auth = kimi.toAuth(credential);
    EXPECT_FALSE(auth.apiKey.has_value());
    ASSERT_EQ(auth.headers.size(), 1U);
    EXPECT_EQ(auth.headers[0].second, "Bearer tok");
    EXPECT_TRUE(anthropic.isSubscription());
    EXPECT_FALSE(OauthRefreshFlow(spec("openrouter"), m_http, m_clock, m_base64).isSubscription());
    EXPECT_EQ(anthropic.providerId(), "anthropic");
}

TEST_F(OauthRefreshFlowTest, RadiusRefreshesAtItsGatewayAndKeepsTheScope) {
    m_http.enqueue(reply(Json{{"access_token", "new-access"}, {"refresh_token", "new-refresh"}, {"expires_in", 3600}, {"scope", "gateway offline_access"}}));
    const auto result = refresh(BuiltinOauthSpecs().radius("https://gw.example"), oauth("old-refresh"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(m_http.requests()[0].url, "https://gw.example/v1/oauth/token");
    EXPECT_NE(m_http.requests()[0].body.find("client_id=pi-gateway"), std::string::npos);
    EXPECT_NE(m_http.requests()[0].body.find("refresh_token=old-refresh"), std::string::npos);
    EXPECT_EQ(result->access, "new-access");
    EXPECT_EQ(result->extra["scope"], "gateway offline_access");
    EXPECT_DOUBLE_EQ(result->expires, 1'000'000 + 3'600'000 - 60'000);
    EXPECT_FALSE(OauthRefreshFlow(BuiltinOauthSpecs().radius("https://gw.example"), m_http, m_clock, m_base64).isSubscription());
}
