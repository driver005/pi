#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.meta_oauth_flow;
import pi.testing.fixed_clock;
import pi.testing.scripted_http_client;

class MetaOauthFlowTest : public testing::Test {
protected:
    Credential identity() {
        Credential credential;
        credential.type = CredentialType::OAuth;
        credential.refresh = "identity-token";
        credential.access = "old-key";
        return credential;
    }

    HttpResponse reply(const Json& body, int status = 200) {
        HttpResponse response;
        response.status = status;
        response.body = body.dump();
        return response;
    }

    FixedClock m_clock{1'000'000};
    ScriptedHttpClient m_http;
    MetaOauthFlow m_flow{m_http, m_clock};
};

TEST_F(MetaOauthFlowTest, MintsAnApiKeyFromTheIdentityToken) {
    m_http.enqueue(reply(Json{{"api_key", "meta-key"}}));
    const auto result = m_flow.refresh(identity(), nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->access, "meta-key");
    EXPECT_EQ(result->refresh, "identity-token");
    EXPECT_DOUBLE_EQ(result->expires, 1'000'000 + 24.0 * 60 * 60 * 1000);
    EXPECT_EQ(m_flow.toAuth(*result).apiKey, "meta-key");
    ASSERT_EQ(m_http.requests().size(), 1U);
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://api.meta.ai/muse-code/key");
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.body, "{}");
    bool bearer = false;
    for (const auto& [name, value] : request.headers) {
        bearer = bearer || (name == "Authorization" && value == "Bearer identity-token");
    }
    EXPECT_TRUE(bearer);
}

TEST_F(MetaOauthFlowTest, ADeadSessionAsksForANewSignIn) {
    m_http.enqueue(reply(Json{{"error_description", "token revoked"}}, 401));
    const auto result = m_flow.refresh(identity(), nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("Meta session expired (status 401)"), std::string::npos);
    EXPECT_NE(result.error().message.find("pi auth login meta"), std::string::npos);
    EXPECT_NE(result.error().message.find(": token revoked"), std::string::npos);
}

TEST_F(MetaOauthFlowTest, OtherFailuresAreReported) {
    m_http.enqueue(reply(Json{{"message", "  down  "}}, 500));
    auto result = m_flow.refresh(identity(), nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Meta API key mint failed with status 500: down");

    m_http.enqueue(reply(Json{{"action_url", "https://meta.example/setup"}}));
    result = m_flow.refresh(identity(), nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Meta did not issue an API key. Complete setup at https://meta.example/setup");

    m_http.enqueue(reply(Json{{"action_url", "javascript:alert(1)"}}));
    result = m_flow.refresh(identity(), nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Meta did not issue an API key.");
}
