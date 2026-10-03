#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.base.boring_crypto;
import pi.support.mcp_oauth_token_provider;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.scripted_http_client;

class PassthroughLock : public IFileLock {
public:
    Result<void> withLock(const std::string&, const std::function<Result<void>()>& action) override {
        return action();
    }
};

class McpOauthTokenProviderTest : public testing::Test {
protected:
    static constexpr const char* kServer = "https://mcp.example.com/mcp";

    McpOauthTokenProviderTest() {
        m_files.createDirectories("/agent");
    }

    void store(const Json& tokens, std::optional<std::int64_t> expiresAt = std::nullopt) {
        Json state{{"serverUrl", kServer},
                   {"clientInformation", Json{{"client_id", "c1"}}},
                   {"discovery", Json{{"authorizationServerUrl", "https://auth.example.com"},
                                      {"authorizationServerMetadata", Json{{"issuer", "https://auth.example.com"},
                                                                           {"authorization_endpoint", "https://auth.example.com/authorize"},
                                                                           {"token_endpoint", "https://auth.example.com/token"},
                                                                           {"response_types_supported", Json::array({"code"})}}}}},
                   {"tokens", tokens}};
        if (expiresAt) {
            state["tokensExpireAt"] = *expiresAt;
        }
        ASSERT_TRUE(m_store.save("docs", kServer, state).has_value());
    }

    HttpResponse reply(int status, const Json& body) {
        HttpResponse response;
        response.status = status;
        response.body = body.dump();
        return response;
    }

    Json stored() {
        return **m_store.load("docs", kServer);
    }

    FakeFileSystem m_files;
    PassthroughLock m_lock;
    BoringCrypto m_crypto;
    Base64Codec m_base64;
    ScriptedHttpClient m_http;
    FixedClock m_clock{10'000'000};
    McpOauthStore m_store{"/agent/mcp-auth.json", "/agent", m_files, m_lock, m_crypto};
    McpOauthRefresher m_refresher{m_http, m_clock, m_base64};
    McpOauthTokenProvider m_provider{"docs", kServer, m_store, m_refresher, m_clock, []() -> Result<Json> { return Json::object(); }};
};

TEST_F(McpOauthTokenProviderTest, WithoutStoredStateThereIsNoToken) {
    EXPECT_EQ(m_provider.token(), "");
}

TEST_F(McpOauthTokenProviderTest, ASoundTokenIsSentAsStored) {
    store(Json{{"access_token", "tok"}, {"token_type", "Bearer"}, {"refresh_token", "r"}}, 10'000'000 + 3'600'000);
    EXPECT_EQ(m_provider.token(), "tok");
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(McpOauthTokenProviderTest, ATokenAboutToExpireIsRefreshedFirst) {
    store(Json{{"access_token", "old"}, {"token_type", "Bearer"}, {"refresh_token", "r"}}, 10'000'000 + 20'000);
    m_http.enqueue(reply(200, Json{{"access_token", "fresh"}, {"token_type", "Bearer"}, {"expires_in", 3600}, {"refresh_token", "r2"}}));
    EXPECT_EQ(m_provider.token(), "fresh");
    EXPECT_EQ(stored()["tokens"]["refresh_token"], "r2");
    EXPECT_EQ(stored()["tokensExpireAt"], 10'000'000 + 3'600'000);
}

TEST_F(McpOauthTokenProviderTest, AFailedRefreshBeforeSendingFallsBackToTheOldToken) {
    store(Json{{"access_token", "old"}, {"token_type", "Bearer"}, {"refresh_token", "r"}}, 10'000'000 - 1);
    m_http.enqueue(std::unexpected(Error{"timeout", "slow"}));
    EXPECT_EQ(m_provider.token(), "old");
}

TEST_F(McpOauthTokenProviderTest, ATokenWithoutARefreshTokenIsSentEvenWhenExpired) {
    store(Json{{"access_token", "old"}, {"token_type", "Bearer"}}, 10'000'000 - 1);
    EXPECT_EQ(m_provider.token(), "old");
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(McpOauthTokenProviderTest, A401RefreshesAndAsksForARetry) {
    store(Json{{"access_token", "old"}, {"token_type", "Bearer"}, {"refresh_token", "r"}});
    m_http.enqueue(reply(200, Json{{"access_token", "fresh"}, {"token_type", "Bearer"}}));
    EXPECT_TRUE(m_provider.onUnauthorized("Bearer realm=\"mcp\"", "old"));
    EXPECT_EQ(m_provider.token(), "fresh");
}

TEST_F(McpOauthTokenProviderTest, AnAlreadyReplacedTokenIsJustRetried) {
    store(Json{{"access_token", "newer"}, {"token_type", "Bearer"}, {"refresh_token", "r"}});
    EXPECT_TRUE(m_provider.onUnauthorized("Bearer", "old"));
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(McpOauthTokenProviderTest, InsufficientScopeNeedsANewSignIn) {
    store(Json{{"access_token", "old"}, {"token_type", "Bearer"}, {"refresh_token", "r"}});
    EXPECT_FALSE(m_provider.onUnauthorized("Bearer error=\"insufficient_scope\", scope=\"write\"", "old"));
    EXPECT_EQ(m_http.calls(), 0);
    EXPECT_EQ(m_provider.challenge()["scope"], "write");
    EXPECT_EQ(m_provider.challenge()["error"], "insufficient_scope");
}

TEST_F(McpOauthTokenProviderTest, ARejectedGrantDropsTheStoredTokens) {
    store(Json{{"access_token", "old"}, {"token_type", "Bearer"}, {"refresh_token", "r"}});
    m_http.enqueue(reply(400, Json{{"error", "invalid_grant"}}));
    EXPECT_FALSE(m_provider.onUnauthorized("Bearer", "old"));
    EXPECT_FALSE(stored().contains("tokens"));
    EXPECT_TRUE(stored().contains("clientInformation"));
    EXPECT_EQ(m_provider.token(), "");
}

TEST_F(McpOauthTokenProviderTest, WithoutARefreshTokenA401NeedsASignIn) {
    store(Json{{"access_token", "old"}, {"token_type", "Bearer"}});
    EXPECT_FALSE(m_provider.onUnauthorized("Bearer", "old"));
    EXPECT_EQ(stored()["tokens"]["access_token"], "old");
}

TEST_F(McpOauthTokenProviderTest, SettingsFailuresFailTheRefreshNotTheProvider) {
    McpOauthTokenProvider provider("docs", kServer, m_store, m_refresher, m_clock, []() -> Result<Json> { return std::unexpected(Error{"secret", "cannot resolve"}); });
    store(Json{{"access_token", "old"}, {"token_type", "Bearer"}, {"refresh_token", "r"}}, 10'000'000 - 1);
    EXPECT_EQ(provider.token(), "old");
    EXPECT_FALSE(provider.onUnauthorized("Bearer", "old"));
}

TEST_F(McpOauthTokenProviderTest, ParsesChallenges) {
    const Json parsed = m_provider.parseChallenge("Bearer realm=\"x\", resource_metadata=\"https://mcp.example.com/.well-known/oauth-protected-resource\", scope=\"a b\", error=invalid_token, error_description=\"expired\"");
    EXPECT_EQ(parsed["resourceMetadataUrl"], "https://mcp.example.com/.well-known/oauth-protected-resource");
    EXPECT_EQ(parsed["scope"], "a b");
    EXPECT_EQ(parsed["error"], "invalid_token");
    EXPECT_EQ(parsed["errorDescription"], "expired");
    EXPECT_EQ(m_provider.parseChallenge("Basic realm=\"x\""), Json::object());
    EXPECT_EQ(m_provider.parseChallenge(""), Json::object());
    EXPECT_FALSE(m_provider.parseChallenge("DPoP scope=\"\"").contains("scope"));
}
