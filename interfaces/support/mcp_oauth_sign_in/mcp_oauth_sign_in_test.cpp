#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.base.boring_crypto;
import pi.support.header_merger;
import pi.support.mcp_oauth_sign_in;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.scripted_http_client;

class PassthroughLock : public IFileLock {
public:
    Result<void> withLock(const std::string&, const std::function<Result<void>()>& action) override {
        return action();
    }
};

/** What the sign-in asked of its callback servers. */
struct CallbackRecord {
    std::string host;
    int requestedPort = -1;
    bool required = false;
    std::vector<std::string> paths;
    int closes = 0;
    int boundPort = 41234;
    /** Builds the browser's answer from the state the sign-in is waiting for. */
    std::function<Result<Json>(const std::string& state)> answer;
};

class FakeCallbackServer : public ICallbackServer {
public:
    explicit FakeCallbackServer(CallbackRecord& record)
        : m_record(record) {}

    Result<int> listen(const std::string& host, int port, bool required) override {
        m_record.host = host;
        m_record.requestedPort = port;
        m_record.required = required;
        return m_record.boundPort;
    }
    Result<Json> waitForCallback(const std::vector<std::string>& paths, const std::string& state, std::chrono::milliseconds, const std::shared_ptr<AbortSignal>&) override {
        m_record.paths = paths;
        return m_record.answer(state);
    }
    void close() override {
        ++m_record.closes;
    }

    CallbackRecord& m_record;
};

class McpOauthSignInTest : public testing::Test {
protected:
    static constexpr const char* kServer = "https://mcp.example.com/mcp";

    McpOauthSignInTest() {
        m_files.createDirectories("/agent");
        m_callback.answer = [](const std::string& state) -> Result<Json> { return Json{{"code", "auth-code"}, {"state", state}}; };
    }

    HttpResponse reply(int status, const Json& body) {
        HttpResponse response;
        response.status = status;
        response.body = body.dump();
        return response;
    }

    Json resourceMetadata(const Json& extra = Json::object()) {
        Json value{{"resource", "https://mcp.example.com/mcp"}, {"authorization_servers", Json::array({"https://auth.example.com"})}};
        for (const auto& entry : extra.items()) {
            value[entry.key()] = entry.value();
        }
        return value;
    }

    Json serverMetadata(const Json& extra = Json::object()) {
        Json metadata{{"issuer", "https://auth.example.com"},
                      {"authorization_endpoint", "https://auth.example.com/authorize"},
                      {"token_endpoint", "https://auth.example.com/token"},
                      {"registration_endpoint", "https://auth.example.com/register"},
                      {"response_types_supported", Json::array({"code"})},
                      {"code_challenge_methods_supported", Json::array({"S256"})}};
        for (const auto& entry : extra.items()) {
            if (entry.value().is_null()) {
                metadata.erase(entry.key());
            } else {
                metadata[entry.key()] = entry.value();
            }
        }
        return metadata;
    }

    /** Resource metadata, server metadata, registration, token answers for a fresh sign-in. */
    void scriptFreshSignIn(const Json& metadata = Json(), const Json& resource = Json()) {
        m_http.enqueue(reply(200, resource.is_null() ? resourceMetadata() : resource));
        m_http.enqueue(reply(200, metadata.is_null() ? serverMetadata() : metadata));
        m_http.enqueue(reply(201, Json{{"client_id", "registered-1"}}));
        m_http.enqueue(reply(200, Json{{"access_token", "tok"}, {"token_type", "Bearer"}, {"expires_in", 3600}, {"refresh_token", "r1"}}));
    }

    Result<void> signIn(const Json& settings = Json::object(), const Json& challenge = Json()) {
        return m_signIn.signIn("docs", kServer, settings, challenge, [this](const std::string& url) { m_urls.push_back(url); }, std::chrono::seconds(5), nullptr);
    }

    std::map<std::string, std::string> queryOf(const std::string& url) {
        std::map<std::string, std::string> out;
        for (const auto& [name, value] : m_parser.parseQuery(url.substr(url.find('?') + 1))) {
            out[name] = value;
        }
        return out;
    }

    Json stored() {
        return **m_store.load("docs", kServer);
    }

    FakeFileSystem m_files;
    PassthroughLock m_lock;
    BoringCrypto m_crypto;
    Base64Codec m_base64;
    ScriptedHttpClient m_http;
    FixedClock m_clock{2'000'000};
    CallbackRecord m_callback;
    UrlParser m_parser;
    std::vector<std::string> m_urls;
    McpOauthStore m_store{"/agent/mcp-auth.json", "/agent", m_files, m_lock, m_crypto};
    McpOauthRefresher m_refresher{m_http, m_clock, m_base64};
    McpOauthSignIn m_signIn{m_store, m_refresher, m_http, m_crypto, m_base64, [this]() { return std::unique_ptr<ICallbackServer>(std::make_unique<FakeCallbackServer>(m_callback)); }};
};

TEST_F(McpOauthSignInTest, SignsInWithDiscoveryRegistrationAndTheCodeExchange) {
    scriptFreshSignIn(Json(), resourceMetadata(Json{{"scopes_supported", Json::array({"read", "write"})}}));
    const auto done = signIn();
    ASSERT_TRUE(done.has_value()) << done.error().message;
    ASSERT_EQ(m_urls.size(), 1u);
    const auto query = queryOf(m_urls[0]);
    EXPECT_TRUE(m_urls[0].starts_with("https://auth.example.com/authorize?"));
    EXPECT_EQ(query.at("response_type"), "code");
    EXPECT_EQ(query.at("client_id"), "registered-1");
    EXPECT_EQ(query.at("redirect_uri"), "http://127.0.0.1:41234/callback");
    EXPECT_EQ(query.at("code_challenge_method"), "S256");
    EXPECT_EQ(query.at("scope"), "read write");
    EXPECT_EQ(query.at("resource"), "https://mcp.example.com/mcp");
    EXPECT_EQ(query.at("state").size(), 64u);
    EXPECT_EQ(m_callback.host, "127.0.0.1");
    EXPECT_EQ(m_callback.requestedPort, 0);
    EXPECT_FALSE(m_callback.required);
    EXPECT_EQ(m_callback.paths, std::vector<std::string>{"/callback"});
    EXPECT_EQ(m_callback.closes, 1);

    const std::vector<HttpRequest> requests = m_http.requests();
    ASSERT_EQ(requests.size(), 4u);
    const Json registration = Json::parse(requests[2].body);
    EXPECT_EQ(requests[2].url, "https://auth.example.com/register");
    EXPECT_EQ(registration["redirect_uris"][0], "http://127.0.0.1:41234/callback");
    EXPECT_EQ(registration["token_endpoint_auth_method"], "none");
    EXPECT_EQ(registration["scope"], "read write");
    EXPECT_EQ(requests[3].url, "https://auth.example.com/token");
    const auto form = m_parser.parseQuery(requests[3].body);
    std::map<std::string, std::string> token(form.begin(), form.end());
    EXPECT_EQ(token["grant_type"], "authorization_code");
    EXPECT_EQ(token["code"], "auth-code");
    EXPECT_EQ(token["redirect_uri"], "http://127.0.0.1:41234/callback");
    EXPECT_EQ(token["client_id"], "registered-1");
    // PKCE: the challenge in the URL is the S256 of the verifier in the token request.
    EXPECT_EQ(query.at("code_challenge"), m_base64.encodeUrl(m_crypto.sha256(token["code_verifier"])));

    const Json state = stored();
    EXPECT_EQ(state["tokens"]["access_token"], "tok");
    EXPECT_EQ(state["tokens"]["refresh_token"], "r1");
    EXPECT_EQ(state["tokens"]["scope"], "read write");
    EXPECT_EQ(state["tokensExpireAt"], 2'000'000 + 3'600'000);
    EXPECT_EQ(state["clientInformation"]["client_id"], "registered-1");
    EXPECT_EQ(state["clientInformation"]["redirect_uris"][0], "http://127.0.0.1:41234/callback");
    EXPECT_EQ(state["discovery"]["authorizationServerUrl"], "https://auth.example.com");
    EXPECT_FALSE(state.contains("codeVerifier"));
    EXPECT_FALSE(state.contains("oauthState"));
}

TEST_F(McpOauthSignInTest, ARefreshableGrantSignsInWithoutTheBrowser) {
    const Json discovery{{"authorizationServerUrl", "https://auth.example.com"}, {"authorizationServerMetadata", serverMetadata()}};
    ASSERT_TRUE(m_store.save("docs", kServer, Json{{"serverUrl", kServer}, {"clientInformation", Json{{"client_id", "c1"}, {"redirect_uris", Json::array({"http://127.0.0.1:41234/callback"})}}},
                                                   {"tokens", Json{{"access_token", "old"}, {"token_type", "Bearer"}, {"refresh_token", "r"}}}, {"discovery", discovery}}).has_value());
    m_http.enqueue(reply(200, Json{{"access_token", "fresh"}, {"token_type", "Bearer"}}));
    ASSERT_TRUE(signIn().has_value());
    EXPECT_TRUE(m_urls.empty());
    EXPECT_EQ(stored()["tokens"]["access_token"], "fresh");
    EXPECT_EQ(m_callback.closes, 1);
}

TEST_F(McpOauthSignInTest, AStepUpSkipsTheRefreshAndRequestsGrantedAndChallengedScopes) {
    const Json discovery{{"authorizationServerUrl", "https://auth.example.com"}, {"authorizationServerMetadata", serverMetadata()}};
    ASSERT_TRUE(m_store.save("docs", kServer, Json{{"serverUrl", kServer}, {"clientInformation", Json{{"client_id", "c1"}, {"redirect_uris", Json::array({"http://127.0.0.1:41234/callback"})}}},
                                                   {"tokens", Json{{"access_token", "old"}, {"token_type", "Bearer"}, {"refresh_token", "r"}, {"scope", "read"}}}, {"discovery", discovery}}).has_value());
    m_http.enqueue(reply(200, Json{{"access_token", "wide"}, {"token_type", "Bearer"}, {"scope", "read write"}}));
    const Json challenge{{"error", "insufficient_scope"}, {"scope", "write"}};
    ASSERT_TRUE(signIn(Json{{"scope", "extra"}}, challenge).has_value());
    ASSERT_EQ(m_urls.size(), 1u);
    EXPECT_EQ(queryOf(m_urls[0]).at("scope"), "extra read write");
    EXPECT_EQ(queryOf(m_urls[0]).at("client_id"), "c1");
    EXPECT_EQ(m_http.calls(), 1);
    EXPECT_EQ(stored()["tokens"]["scope"], "read write");
}

TEST_F(McpOauthSignInTest, AConfiguredClientIsNotRegistered) {
    m_http.enqueue(reply(200, resourceMetadata()));
    m_http.enqueue(reply(200, serverMetadata()));
    m_http.enqueue(reply(200, Json{{"access_token", "tok"}, {"token_type", "Bearer"}}));
    ASSERT_TRUE(signIn(Json{{"clientId", "mine"}, {"clientSecret", "shh"}}).has_value());
    EXPECT_EQ(queryOf(m_urls[0]).at("client_id"), "mine");
    ASSERT_EQ(m_http.calls(), 3);
    // Without advertised methods a client with a secret authenticates with HTTP basic.
    const HttpRequest exchange = m_http.requests()[2];
    EXPECT_EQ(HeaderMerger().find(exchange.headers, "authorization"), std::optional<std::string>("Basic " + m_base64.encode("mine:shh")));
    EXPECT_EQ(exchange.body.find("client_id"), std::string::npos);
}

TEST_F(McpOauthSignInTest, AServerWithoutRegistrationNeedsAConfiguredClient) {
    m_http.enqueue(reply(200, resourceMetadata()));
    m_http.enqueue(reply(200, serverMetadata(Json{{"registration_endpoint", nullptr}})));
    const auto done = signIn();
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, "oauth_registration");
    EXPECT_NE(done.error().message.find("oauth.clientId"), std::string::npos);
    EXPECT_TRUE(m_urls.empty());
    EXPECT_EQ(m_callback.closes, 1);
}

TEST_F(McpOauthSignInTest, ARejectedRegistrationIsReported) {
    m_http.enqueue(reply(200, resourceMetadata()));
    m_http.enqueue(reply(200, serverMetadata()));
    m_http.enqueue(reply(400, Json{{"error", "invalid_redirect_uri"}}));
    const auto done = signIn();
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, "oauth_registration");
    EXPECT_NE(done.error().message.find("400"), std::string::npos);
}

TEST_F(McpOauthSignInTest, AuthorizationErrorsFromTheBrowserAreReported) {
    scriptFreshSignIn();
    m_callback.answer = [](const std::string& state) -> Result<Json> { return Json{{"error", "access_denied"}, {"error_description", "User said no"}, {"state", state}}; };
    const auto done = signIn();
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, "oauth:access_denied");
    EXPECT_EQ(done.error().message, "User said no");
    EXPECT_EQ(m_http.calls(), 3);
    EXPECT_FALSE(stored().contains("tokens"));
}

TEST_F(McpOauthSignInTest, TheCallbackServersFailuresPassThrough) {
    scriptFreshSignIn();
    m_callback.answer = [](const std::string&) -> Result<Json> { return std::unexpected(Error{"timeout", "Timed out"}); };
    const auto done = signIn();
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, "timeout");
    EXPECT_EQ(m_callback.closes, 1);
}

TEST_F(McpOauthSignInTest, AnIssuerThatDiffersFromTheMetadataIsRefused) {
    scriptFreshSignIn();
    m_callback.answer = [](const std::string& state) -> Result<Json> { return Json{{"code", "c"}, {"state", state}, {"iss", "https://evil.example.com"}}; };
    const auto done = signIn();
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, "oauth_issuer");
    EXPECT_EQ(m_http.calls(), 3);
}

TEST_F(McpOauthSignInTest, AMissingIssuerIsRefusedWhenTheServerPromisedOne) {
    scriptFreshSignIn(serverMetadata(Json{{"authorization_response_iss_parameter_supported", true}}));
    const auto done = signIn();
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, "oauth_issuer");
}

TEST_F(McpOauthSignInTest, ACorrectIssuerIsAccepted) {
    scriptFreshSignIn(serverMetadata(Json{{"authorization_response_iss_parameter_supported", true}}));
    m_callback.answer = [](const std::string& state) -> Result<Json> { return Json{{"code", "c"}, {"state", state}, {"iss", "https://auth.example.com"}}; };
    EXPECT_TRUE(signIn().has_value());
}

TEST_F(McpOauthSignInTest, AConfiguredCallbackPortIsRequiredAndFixesTheRedirectUri) {
    scriptFreshSignIn();
    ASSERT_TRUE(signIn(Json{{"callbackPort", 8765}}).has_value());
    EXPECT_EQ(m_callback.requestedPort, 8765);
    EXPECT_TRUE(m_callback.required);
    EXPECT_EQ(queryOf(m_urls[0]).at("redirect_uri"), "http://127.0.0.1:8765/callback");
}

TEST_F(McpOauthSignInTest, AConfiguredCallbackUrlIsUsedAsWritten) {
    scriptFreshSignIn();
    ASSERT_TRUE(signIn(Json{{"callbackUrl", "http://localhost:9000/cb"}}).has_value());
    EXPECT_EQ(m_callback.host, "127.0.0.1");
    EXPECT_EQ(m_callback.requestedPort, 9000);
    EXPECT_EQ(m_callback.paths, std::vector<std::string>{"/cb"});
    EXPECT_EQ(queryOf(m_urls[0]).at("redirect_uri"), "http://localhost:9000/cb");
}

TEST_F(McpOauthSignInTest, AnIpv6CallbackUrlIsBracketedInTheRedirectUri) {
    scriptFreshSignIn();
    ASSERT_TRUE(signIn(Json{{"callbackUrl", "http://[::1]/callback"}}).has_value());
    EXPECT_EQ(m_callback.host, "::1");
    EXPECT_EQ(queryOf(m_urls[0]).at("redirect_uri"), "http://[::1]:41234/callback");
}

TEST_F(McpOauthSignInTest, ARegisteredClientOfAnotherRedirectUriIsReplaced) {
    ASSERT_TRUE(m_store.save("docs", kServer, Json{{"serverUrl", kServer}, {"clientInformation", Json{{"client_id", "old-client"}, {"redirect_uris", Json::array({"http://127.0.0.1:9999/callback"})}}},
                                                   {"tokens", Json{{"access_token", "old"}, {"token_type", "Bearer"}}}}).has_value());
    scriptFreshSignIn();
    ASSERT_TRUE(signIn().has_value());
    // The port of the registered redirect URI was preferred, but the bound port differs, so a new client was registered.
    EXPECT_EQ(m_callback.requestedPort, 9999);
    EXPECT_EQ(stored()["clientInformation"]["client_id"], "registered-1");
    EXPECT_EQ(stored()["tokens"]["access_token"], "tok");
}

TEST_F(McpOauthSignInTest, ARegisteredClientWithTheSameRedirectUriIsKept) {
    m_callback.boundPort = 9999;
    const Json discovery{{"authorizationServerUrl", "https://auth.example.com"}, {"authorizationServerMetadata", serverMetadata()}};
    ASSERT_TRUE(m_store.save("docs", kServer, Json{{"serverUrl", kServer}, {"clientInformation", Json{{"client_id", "kept"}, {"redirect_uris", Json::array({"http://127.0.0.1:9999/callback"})}}}, {"discovery", discovery}}).has_value());
    m_http.enqueue(reply(200, Json{{"access_token", "tok"}, {"token_type", "Bearer"}}));
    ASSERT_TRUE(signIn().has_value());
    EXPECT_EQ(queryOf(m_urls[0]).at("client_id"), "kept");
    EXPECT_EQ(m_http.calls(), 1);
}

TEST_F(McpOauthSignInTest, AuthorizationServersThatLackCodeAreRefused) {
    scriptFreshSignIn(serverMetadata(Json{{"response_types_supported", Json::array({"token"})}}));
    const auto done = signIn();
    ASSERT_FALSE(done.has_value());
    EXPECT_NE(done.error().message.find("authorization codes"), std::string::npos);
}

TEST_F(McpOauthSignInTest, AuthorizationServersThatLackPkceAreRefused) {
    scriptFreshSignIn(serverMetadata(Json{{"code_challenge_methods_supported", Json::array({"plain"})}}));
    const auto done = signIn();
    ASSERT_FALSE(done.has_value());
    EXPECT_NE(done.error().message.find("PKCE"), std::string::npos);
}

TEST_F(McpOauthSignInTest, OfflineAccessAsksForConsent) {
    scriptFreshSignIn();
    ASSERT_TRUE(signIn(Json{{"scope", "read offline_access"}}).has_value());
    EXPECT_EQ(queryOf(m_urls[0]).at("prompt"), "consent");
}

TEST_F(McpOauthSignInTest, ClientIdMetadataDocumentsAreNotSupported) {
    const auto done = signIn(Json{{"clientRegistration", "cimd"}});
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, "oauth_registration");
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(McpOauthSignInTest, InvalidCallbackUrlsAreReported) {
    const auto done = signIn(Json{{"callbackUrl", "nonsense"}});
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, "oauth_settings");
}
