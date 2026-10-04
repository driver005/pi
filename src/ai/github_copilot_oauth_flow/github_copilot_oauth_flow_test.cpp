#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.github_copilot_oauth_flow;
import pi.testing.scripted_http_client;

class GithubCopilotOauthFlowTest : public testing::Test {
protected:
    Credential stored(const std::string& enterprise = "") {
        Credential credential;
        credential.type = CredentialType::OAuth;
        credential.refresh = "gh-token";
        credential.access = "old";
        credential.extra["availableModelIds"] = Json::array({"gpt-4o"});
        if (!enterprise.empty()) {
            credential.extra["enterpriseUrl"] = enterprise;
        }
        return credential;
    }

    HttpResponse reply(const Json& body, int status = 200) {
        HttpResponse response;
        response.status = status;
        response.body = body.dump();
        return response;
    }

    ScriptedHttpClient m_http;
    GithubCopilotOauthFlow m_flow{m_http};
};

TEST_F(GithubCopilotOauthFlowTest, ExchangesTheGithubTokenForACopilotToken) {
    m_http.enqueue(reply(Json{{"token", "tid=1;proxy-ep=proxy.individual.githubcopilot.com;x=y"}, {"expires_at", 2000}}));
    const auto result = m_flow.refresh(stored(), nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->access, "tid=1;proxy-ep=proxy.individual.githubcopilot.com;x=y");
    EXPECT_EQ(result->refresh, "gh-token");
    EXPECT_DOUBLE_EQ(result->expires, 2000 * 1000 - 5 * 60 * 1000);
    EXPECT_EQ(result->extra["availableModelIds"], Json::array({"gpt-4o"}));
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://api.github.com/copilot_internal/v2/token");
    bool sawAuth = false;
    for (const auto& header : request.headers) {
        sawAuth = sawAuth || (header.first == "Authorization" && header.second == "Bearer gh-token");
    }
    EXPECT_TRUE(sawAuth);
}

TEST_F(GithubCopilotOauthFlowTest, EnterpriseDomainsChangeTheEndpoint) {
    m_http.enqueue(reply(Json{{"token", "t"}, {"expires_at", 10}}));
    ASSERT_TRUE(m_flow.refresh(stored("https://octo.example.com/"), nullptr).has_value());
    EXPECT_EQ(m_http.requests()[0].url, "https://api.octo.example.com/copilot_internal/v2/token");
}

TEST_F(GithubCopilotOauthFlowTest, BaseUrlComesFromTheTokenThenTheDomain) {
    Credential credential = stored();
    credential.access = "tid=1;proxy-ep=proxy.business.githubcopilot.com;exp=2";
    EXPECT_EQ(m_flow.toAuth(credential).baseUrl, "https://api.business.githubcopilot.com");
    EXPECT_EQ(m_flow.toAuth(credential).apiKey, credential.access);
    Credential plain = stored();
    plain.access = "opaque";
    EXPECT_EQ(m_flow.toAuth(plain).baseUrl, "https://api.individual.githubcopilot.com");
    Credential enterprise = stored("octo.example.com");
    enterprise.access = "opaque";
    EXPECT_EQ(m_flow.toAuth(enterprise).baseUrl, "https://copilot-api.octo.example.com");
}

TEST_F(GithubCopilotOauthFlowTest, FailuresAreReported) {
    m_http.enqueue(reply(Json{{"message", "Bad credentials"}}, 401));
    const auto rejected = m_flow.refresh(stored(), nullptr);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_NE(rejected.error().message.find("(401)"), std::string::npos);
    m_http.enqueue(reply(Json{{"token", 5}}));
    EXPECT_EQ(m_flow.refresh(stored(), nullptr).error().message, "Invalid Copilot token response fields");
    EXPECT_FALSE(m_flow.refresh(stored(), nullptr).has_value());
    EXPECT_TRUE(m_flow.isSubscription());
    EXPECT_EQ(m_flow.providerId(), "github-copilot");
}
