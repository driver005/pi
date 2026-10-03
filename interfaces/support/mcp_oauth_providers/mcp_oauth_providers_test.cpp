#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.base.boring_crypto;
import pi.support.mcp_oauth_providers;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.scripted_http_client;
import pi.testing.scripted_process_runner;

class PassthroughLock : public IFileLock {
public:
    Result<void> withLock(const std::string&, const std::function<Result<void>()>& action) override {
        return action();
    }
};

class McpOauthProvidersTest : public testing::Test {
protected:
    McpOauthProvidersTest()
        : m_environment({{"OAUTH_SECRET", "s3cret"}}),
          m_runner([](const ProcessRequest&) -> Result<ProcessResult> { return ProcessResult{}; }),
          m_resolver(m_environment, m_runner) {}

    McpServerConfig remote(const std::string& name = "docs") {
        McpServerConfig config;
        config.name = name;
        config.http = true;
        config.url = "https://mcp.example.com/mcp";
        return config;
    }

    FakeFileSystem m_files;
    FakeEnvironment m_environment;
    ScriptedProcessRunner m_runner;
    ConfigValueResolver m_resolver;
    PassthroughLock m_lock;
    BoringCrypto m_crypto;
    Base64Codec m_base64;
    ScriptedHttpClient m_http;
    FixedClock m_clock{5'000'000};
    McpOauthStore m_store{"/agent/mcp-auth.json", "/agent", m_files, m_lock, m_crypto};
    McpOauthRefresher m_refresher{m_http, m_clock, m_base64};
    McpOauthProviders m_providers{m_store, m_refresher, m_clock, m_resolver};
};

TEST_F(McpOauthProvidersTest, HttpServersUseOauthUnlessTheyBringTheirOwnCredentials) {
    EXPECT_TRUE(m_providers.usesOauth(remote()));
    McpServerConfig stdio;
    stdio.name = "local";
    EXPECT_FALSE(m_providers.usesOauth(stdio));
    McpServerConfig withProvider = remote();
    withProvider.authProvider = "github";
    EXPECT_FALSE(m_providers.usesOauth(withProvider));
    McpServerConfig withHeader = remote();
    withHeader.headers["authorization"] = "Bearer x";
    EXPECT_FALSE(m_providers.usesOauth(withHeader));
    McpServerConfig otherHeader = remote();
    otherHeader.headers["X-Team"] = "pi";
    EXPECT_TRUE(m_providers.usesOauth(otherHeader));
    McpServerConfig upper = remote();
    upper.headers["AUTHORIZATION"] = "Bearer x";
    EXPECT_FALSE(m_providers.usesOauth(upper));
}

TEST_F(McpOauthProvidersTest, AProviderIsMadeOncePerServerAndUrl) {
    const auto first = m_providers.providerFor(remote());
    EXPECT_EQ(first, m_providers.providerFor(remote()));
    EXPECT_NE(first, m_providers.providerFor(remote("other")));
    McpServerConfig moved = remote();
    moved.url = "https://elsewhere.example.com/mcp";
    EXPECT_NE(first, m_providers.providerFor(moved));
}

TEST_F(McpOauthProvidersTest, ProvidersReadTheStoredTokenAndResolveTheClientSecretWhenRefreshing) {
    m_files.createDirectories("/agent");
    McpServerConfig config = remote();
    config.raw = Json{{"url", config.url}, {"oauth", Json{{"clientId", "cfg"}, {"clientSecret", "$OAUTH_SECRET"}}}};
    const Json state{{"serverUrl", config.url},
                     {"tokens", Json{{"access_token", "old"}, {"token_type", "Bearer"}, {"refresh_token", "r"}}},
                     {"tokensExpireAt", 5'000'000 - 1},
                     {"discovery", Json{{"authorizationServerUrl", "https://auth.example.com"},
                                        {"authorizationServerMetadata", Json{{"issuer", "https://auth.example.com"}, {"authorization_endpoint", "https://auth.example.com/a"}, {"token_endpoint", "https://auth.example.com/token"}, {"response_types_supported", Json::array({"code"})}, {"token_endpoint_auth_methods_supported", Json::array({"client_secret_post"})}}}}}};
    ASSERT_TRUE(m_store.save("docs", config.url, state).has_value());
    HttpResponse reply;
    reply.status = 200;
    reply.body = Json{{"access_token", "fresh"}, {"token_type", "Bearer"}}.dump();
    m_http.enqueue(reply);
    EXPECT_EQ(m_providers.providerFor(config)->token(), "fresh");
    EXPECT_NE(m_http.requests()[0].body.find("client_id=cfg&client_secret=s3cret"), std::string::npos);
}
