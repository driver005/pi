#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.support.mcp_oauth_refresher;
import pi.testing.fixed_clock;
import pi.testing.scripted_http_client;

class McpOauthRefresherTest : public testing::Test {
protected:
    static constexpr const char* kServer = "https://mcp.example.com/mcp";

    HttpResponse reply(int status, const Json& body) {
        HttpResponse response;
        response.status = status;
        response.body = body.dump();
        return response;
    }

    Json resourceMetadata() {
        return Json{{"resource", "https://mcp.example.com/mcp"}, {"authorization_servers", Json::array({"https://auth.example.com"})}};
    }

    Json serverMetadata(const Json& extra = Json::object()) {
        Json metadata{{"issuer", "https://auth.example.com"},
                      {"authorization_endpoint", "https://auth.example.com/authorize"},
                      {"token_endpoint", "https://auth.example.com/token"},
                      {"response_types_supported", Json::array({"code"})}};
        for (const auto& entry : extra.items()) {
            metadata[entry.key()] = entry.value();
        }
        return metadata;
    }

    Json state(const Json& extra = Json::object()) {
        Json value{{"serverUrl", kServer},
                   {"clientInformation", Json{{"client_id", "client-1"}}},
                   {"tokens", Json{{"access_token", "old"}, {"token_type", "Bearer"}, {"refresh_token", "r1"}, {"scope", "read"}}}};
        for (const auto& entry : extra.items()) {
            value[entry.key()] = entry.value();
        }
        return value;
    }

    /** Resource metadata, authorization server metadata, then the token answer. */
    void scriptDiscovery(const Json& metadata, const HttpResponse& token) {
        m_http.enqueue(reply(200, resourceMetadata()));
        m_http.enqueue(reply(200, metadata));
        m_http.enqueue(token);
    }

    std::string tokenBody() {
        return m_http.requests().back().body;
    }

    ScriptedHttpClient m_http;
    FixedClock m_clock{1'000'000};
    Base64Codec m_base64;
    McpOauthRefresher m_refresher{m_http, m_clock, m_base64};
    std::shared_ptr<AbortSignal> m_signal = std::make_shared<AbortSignal>();
};

TEST_F(McpOauthRefresherTest, DiscoversTheServerRefreshesAndSavesTheNewTokens) {
    scriptDiscovery(serverMetadata(), reply(200, Json{{"access_token", "new"}, {"token_type", "Bearer"}, {"expires_in", 3600}, {"refresh_token", "r2"}}));
    const auto refreshed = m_refresher.refresh(kServer, state(), Json::object(), std::nullopt, m_signal);
    ASSERT_TRUE(refreshed.has_value()) << refreshed.error().message;
    EXPECT_EQ((*refreshed)["tokens"]["access_token"], "new");
    EXPECT_EQ((*refreshed)["tokens"]["refresh_token"], "r2");
    EXPECT_EQ((*refreshed)["tokens"]["scope"], "read");
    EXPECT_EQ((*refreshed)["tokensExpireAt"], 1'000'000 + 3'600'000);
    EXPECT_EQ((*refreshed)["discovery"]["authorizationServerUrl"], "https://auth.example.com");
    const std::vector<HttpRequest> requests = m_http.requests();
    ASSERT_EQ(requests.size(), 3u);
    EXPECT_EQ(requests[0].url, "https://mcp.example.com/.well-known/oauth-protected-resource/mcp");
    EXPECT_EQ(requests[1].url, "https://auth.example.com/.well-known/oauth-authorization-server");
    EXPECT_EQ(requests[2].url, "https://auth.example.com/token");
    EXPECT_EQ(requests[2].method, "POST");
    EXPECT_EQ(tokenBody(), "grant_type=refresh_token&refresh_token=r1&resource=https%3A%2F%2Fmcp.example.com%2Fmcp&client_id=client-1");
}

TEST_F(McpOauthRefresherTest, TheRefreshTokenIsKeptWhenTheServerDoesNotRotateIt) {
    scriptDiscovery(serverMetadata(), reply(200, Json{{"access_token", "new"}, {"token_type", "Bearer"}}));
    const auto refreshed = m_refresher.refresh(kServer, state(), Json::object(), std::nullopt, m_signal);
    ASSERT_TRUE(refreshed.has_value());
    EXPECT_EQ((*refreshed)["tokens"]["refresh_token"], "r1");
    EXPECT_FALSE(refreshed->contains("tokensExpireAt"));
}

TEST_F(McpOauthRefresherTest, CachedDiscoverySkipsTheDiscoveryRequests) {
    m_http.enqueue(reply(200, Json{{"access_token", "new"}, {"token_type", "Bearer"}}));
    const Json cached = Json{{"authorizationServerUrl", "https://auth.example.com"}, {"authorizationServerMetadata", serverMetadata()}};
    const auto refreshed = m_refresher.refresh(kServer, state(Json{{"discovery", cached}}), Json::object(), std::nullopt, m_signal);
    ASSERT_TRUE(refreshed.has_value()) << refreshed.error().message;
    ASSERT_EQ(m_http.calls(), 1);
    EXPECT_EQ(m_http.requests()[0].url, "https://auth.example.com/token");
}

TEST_F(McpOauthRefresherTest, AConfiguredMetadataUrlReplacesDiscovery) {
    m_http.enqueue(reply(200, serverMetadata(Json{{"token_endpoint", "https://auth.example.com/oauth/token"}})));
    m_http.enqueue(reply(404, Json::object()));
    m_http.enqueue(reply(404, Json::object()));
    m_http.enqueue(reply(200, Json{{"access_token", "new"}, {"token_type", "Bearer"}}));
    const auto refreshed = m_refresher.refresh(kServer, state(), Json{{"authServerMetadataUrl", "https://auth.example.com/meta.json"}}, std::nullopt, m_signal);
    ASSERT_TRUE(refreshed.has_value()) << refreshed.error().message;
    EXPECT_EQ(m_http.requests()[0].url, "https://auth.example.com/meta.json");
    EXPECT_EQ(m_http.requests().back().url, "https://auth.example.com/oauth/token");
}

TEST_F(McpOauthRefresherTest, ThePathOfAnIssuerGetsItsOwnWellKnownCandidates) {
    m_http.enqueue(reply(200, Json{{"resource", kServer}, {"authorization_servers", Json::array({"https://auth.example.com/tenant"})}}));
    m_http.enqueue(reply(404, Json::object()));
    m_http.enqueue(reply(404, Json::object()));
    m_http.enqueue(reply(200, serverMetadata(Json{{"issuer", "https://auth.example.com/tenant"}})));
    m_http.enqueue(reply(200, Json{{"access_token", "new"}, {"token_type", "Bearer"}}));
    ASSERT_TRUE(m_refresher.refresh(kServer, state(), Json::object(), std::nullopt, m_signal).has_value());
    const std::vector<HttpRequest> requests = m_http.requests();
    EXPECT_EQ(requests[1].url, "https://auth.example.com/.well-known/oauth-authorization-server/tenant");
    EXPECT_EQ(requests[2].url, "https://auth.example.com/.well-known/openid-configuration/tenant");
    EXPECT_EQ(requests[3].url, "https://auth.example.com/tenant/.well-known/openid-configuration");
}

TEST_F(McpOauthRefresherTest, ChallengeMetadataUrlIsUsedAsGiven) {
    m_http.enqueue(reply(200, resourceMetadata()));
    m_http.enqueue(reply(200, serverMetadata()));
    m_http.enqueue(reply(200, Json{{"access_token", "new"}, {"token_type", "Bearer"}}));
    ASSERT_TRUE(m_refresher.refresh(kServer, state(), Json::object(), std::optional<std::string>("https://mcp.example.com/meta"), m_signal).has_value());
    EXPECT_EQ(m_http.requests()[0].url, "https://mcp.example.com/meta");
}

TEST_F(McpOauthRefresherTest, WithoutARefreshTokenOrClientASignInIsNeeded) {
    Json noRefresh = state();
    noRefresh["tokens"].erase("refresh_token");
    auto result = m_refresher.refresh(kServer, noRefresh, Json::object(), std::nullopt, m_signal);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "auth_required");
    Json noClient = state();
    noClient.erase("clientInformation");
    m_http.enqueue(reply(200, resourceMetadata()));
    m_http.enqueue(reply(200, serverMetadata()));
    result = m_refresher.refresh(kServer, noClient, Json::object(), std::nullopt, m_signal);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "auth_required");
}

TEST_F(McpOauthRefresherTest, RejectedGrantsAndClientsNeedANewSignIn) {
    for (const char* code : {"invalid_grant", "invalid_client", "unauthorized_client"}) {
        scriptDiscovery(serverMetadata(), reply(400, Json{{"error", code}, {"error_description", "no"}}));
        const auto result = m_refresher.refresh(kServer, state(), Json::object(), std::nullopt, m_signal);
        ASSERT_FALSE(result.has_value()) << code;
        EXPECT_EQ(result.error().code, "auth_required") << code;
    }
}

TEST_F(McpOauthRefresherTest, OtherOauthErrorsKeepTheirCodeAndDescription) {
    scriptDiscovery(serverMetadata(), reply(400, Json{{"error", "invalid_scope"}, {"error_description", "scope not allowed"}}));
    auto result = m_refresher.refresh(kServer, state(), Json::object(), std::nullopt, m_signal);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "oauth:invalid_scope");
    EXPECT_EQ(result.error().message, "scope not allowed");
    HttpResponse broken;
    broken.status = 503;
    broken.body = "unavailable";
    scriptDiscovery(serverMetadata(), broken);
    result = m_refresher.refresh(kServer, state(), Json::object(), std::nullopt, m_signal);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "oauth:server_error");
    EXPECT_NE(result.error().message.find("503"), std::string::npos);
}

TEST_F(McpOauthRefresherTest, ARefusesANonHttpsTokenEndpoint) {
    m_http.enqueue(reply(200, resourceMetadata()));
    m_http.enqueue(reply(200, serverMetadata(Json{{"token_endpoint", "http://auth.example.com/token"}})));
    const auto result = m_refresher.refresh(kServer, state(), Json::object(), std::nullopt, m_signal);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "oauth_insecure");
    EXPECT_EQ(m_http.calls(), 2);
}

TEST_F(McpOauthRefresherTest, ClientSecretsAreSentTheWayTheServerAllows) {
    scriptDiscovery(serverMetadata(Json{{"token_endpoint_auth_methods_supported", Json::array({"client_secret_basic"})}}), reply(200, Json{{"access_token", "n"}, {"token_type", "Bearer"}}));
    ASSERT_TRUE(m_refresher.refresh(kServer, state(), Json{{"clientId", "cfg"}, {"clientSecret", "s3cret"}}, std::nullopt, m_signal).has_value());
    const HttpRequest basic = m_http.requests().back();
    EXPECT_EQ(basic.headers.back().first, "Authorization");
    EXPECT_EQ(basic.headers.back().second, "Basic " + m_base64.encode("cfg:s3cret"));
    EXPECT_EQ(basic.body.find("client_id"), std::string::npos);

    scriptDiscovery(serverMetadata(Json{{"token_endpoint_auth_methods_supported", Json::array({"client_secret_post"})}}), reply(200, Json{{"access_token", "n"}, {"token_type", "Bearer"}}));
    ASSERT_TRUE(m_refresher.refresh(kServer, state(), Json{{"clientId", "cfg"}, {"clientSecret", "s3cret"}}, std::nullopt, m_signal).has_value());
    EXPECT_NE(m_http.requests().back().body.find("client_id=cfg&client_secret=s3cret"), std::string::npos);
}

TEST_F(McpOauthRefresherTest, AnIssuerThatDoesNotMatchIsRefused) {
    m_http.enqueue(reply(200, resourceMetadata()));
    m_http.enqueue(reply(200, serverMetadata(Json{{"issuer", "https://evil.example.com"}})));
    const auto result = m_refresher.refresh(kServer, state(), Json::object(), std::nullopt, m_signal);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "oauth_discovery");
    EXPECT_NE(result.error().message.find("issuer mismatch"), std::string::npos);
}

TEST_F(McpOauthRefresherTest, AResourceThatDoesNotCoverTheServerIsRefused) {
    m_http.enqueue(reply(200, Json{{"resource", "https://other.example.com/mcp"}, {"authorization_servers", Json::array({"https://auth.example.com"})}}));
    m_http.enqueue(reply(200, serverMetadata()));
    const auto result = m_refresher.refresh(kServer, state(), Json::object(), std::nullopt, m_signal);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "oauth_discovery");
}

TEST_F(McpOauthRefresherTest, ServersWithoutMetadataFallBackToTheOriginsTokenEndpoint) {
    m_http.enqueue(reply(404, Json::object()));
    m_http.enqueue(reply(404, Json::object()));
    m_http.enqueue(reply(404, Json::object()));
    m_http.enqueue(reply(404, Json::object()));
    m_http.enqueue(reply(200, Json{{"access_token", "n"}, {"token_type", "Bearer"}}));
    const auto result = m_refresher.refresh(kServer, state(), Json::object(), std::nullopt, m_signal);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(m_http.requests().back().url, "https://mcp.example.com/token");
}

TEST_F(McpOauthRefresherTest, TransportFailuresPassThrough) {
    m_http.enqueue(std::unexpected(Error{"timeout", "timed out"}));
    const auto result = m_refresher.refresh(kServer, state(), Json::object(), std::nullopt, m_signal);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "timeout");
}
