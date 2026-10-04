#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.base.boring_crypto;
import pi.support.builtin_oauth_specs;
import pi.support.oauth_browser_login;
import pi.testing.fixed_clock;
import pi.testing.scripted_http_client;

struct CallbackLog {
    std::string host;
    int requestedPort = -1;
    bool required = false;
    std::vector<std::string> paths;
    std::string state;
    int closes = 0;
    int boundPort = 41234;
    bool listenFails = false;
    std::function<Result<Json>(const std::string& state)> answer;
};

class FakeCallbackServer : public ICallbackServer {
public:
    explicit FakeCallbackServer(CallbackLog& log)
        : m_log(log) {}

    Result<int> listen(const std::string& host, int port, bool required) override {
        m_log.host = host;
        m_log.requestedPort = port;
        m_log.required = required;
        if (m_log.listenFails) {
            return std::unexpected(Error{"callback_server", "Unable to listen on port " + std::to_string(port)});
        }
        return port != 0 ? port : m_log.boundPort;
    }

    Result<Json> waitForCallback(const std::vector<std::string>& paths, const std::string& state, std::chrono::milliseconds, const std::shared_ptr<AbortSignal>&) override {
        m_log.paths = paths;
        m_log.state = state;
        return m_log.answer(state);
    }

    void close() override {
        ++m_log.closes;
    }

private:
    CallbackLog& m_log;
};

class OauthBrowserLoginTest : public testing::Test {
protected:
    OauthBrowserLoginTest() {
        m_log.answer = [](const std::string& state) -> Result<Json> { return Json{{"code", "auth-code"}, {"state", state}}; };
        m_interaction.authUrl = [this](const std::string& url, const std::string&) { m_urls.push_back(url); };
    }

    OauthTokenMapper mapperFor(const std::string& providerId) {
        for (const auto& candidate : BuiltinOauthSpecs().all()) {
            if (candidate.providerId == providerId) {
                return OauthTokenMapper(candidate, m_clock, m_base64);
            }
        }
        return OauthTokenMapper(OauthRefreshSpec{}, m_clock, m_base64);
    }

    BrowserLoginSpec anthropic() {
        BrowserLoginSpec spec;
        spec.authorizeUrl = "https://claude.ai/oauth/authorize";
        spec.clientId = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
        spec.scope = "user:profile user:inference";
        spec.port = 53692;
        spec.pasteWhenPortIsTaken = true;
        spec.stateIsVerifier = true;
        spec.authorizeParams = {{"code", "true"}};
        spec.stateInTokenRequest = true;
        spec.copyCodeRedirectUri = "https://platform.claude.com/oauth/code/callback";
        spec.providerName = "Anthropic";
        return spec;
    }

    HttpResponse reply(const Json& body, int status = 200) {
        HttpResponse response;
        response.status = status;
        response.body = body.dump();
        return response;
    }

    std::map<std::string, std::string> queryOf(const std::string& url) {
        std::map<std::string, std::string> out;
        for (const auto& [name, value] : m_parser.parseQuery(url.substr(url.find('?') + 1))) {
            out[name] = value;
        }
        return out;
    }

    Result<Credential> login(const BrowserLoginSpec& spec, const std::string& provider, bool copyCode = false, const std::vector<OauthBrowserLogin::Param>& extra = {}) {
        return m_login.login(spec, mapperFor(provider), extra, copyCode, m_interaction, std::chrono::seconds(5));
    }

    BoringCrypto m_crypto;
    Base64Codec m_base64;
    ScriptedHttpClient m_http;
    FixedClock m_clock{2'000'000};
    UrlParser m_parser;
    CallbackLog m_log;
    LoginInteraction m_interaction;
    std::vector<std::string> m_urls;
    OauthBrowserLogin m_login{m_http, m_crypto, m_base64, [this]() { return std::unique_ptr<ICallbackServer>(std::make_unique<FakeCallbackServer>(m_log)); }};
};

TEST_F(OauthBrowserLoginTest, AnthropicSignsInThroughTheLoopbackCallback) {
    m_http.enqueue(reply(Json{{"access_token", "tok"}, {"refresh_token", "ref"}, {"expires_in", 3600}}));
    const auto credential = login(anthropic(), "anthropic");
    ASSERT_TRUE(credential.has_value()) << credential.error().message;
    EXPECT_EQ(credential->access, "tok");
    EXPECT_EQ(credential->refresh, "ref");
    EXPECT_DOUBLE_EQ(credential->expires, 2'000'000 + 3'600'000 - 300'000);
    EXPECT_EQ(m_log.host, "127.0.0.1");
    EXPECT_EQ(m_log.requestedPort, 53692);
    EXPECT_EQ(m_log.paths, std::vector<std::string>{"/callback"});
    EXPECT_EQ(m_log.closes, 1);

    ASSERT_EQ(m_urls.size(), 1U);
    const auto query = queryOf(m_urls[0]);
    EXPECT_EQ(m_urls[0].substr(0, m_urls[0].find('?')), "https://claude.ai/oauth/authorize");
    EXPECT_EQ(query.at("code"), "true");
    EXPECT_EQ(query.at("response_type"), "code");
    EXPECT_EQ(query.at("client_id"), "9d1c250a-e61b-44d9-88ed-5944d1962f5e");
    EXPECT_EQ(query.at("redirect_uri"), "http://localhost:53692/callback");
    EXPECT_EQ(query.at("scope"), "user:profile user:inference");
    EXPECT_EQ(query.at("code_challenge_method"), "S256");
    // The PKCE verifier doubles as state.
    EXPECT_EQ(m_log.state, query.at("state"));
    const std::string challenge = m_base64.encodeUrl(m_crypto.sha256(query.at("state")));
    EXPECT_EQ(query.at("code_challenge"), challenge);

    const Json sent = Json::parse(m_http.requests()[0].body);
    EXPECT_EQ(sent["grant_type"], "authorization_code");
    EXPECT_EQ(sent["code"], "auth-code");
    EXPECT_EQ(sent["code_verifier"], query.at("state"));
    EXPECT_EQ(sent["state"], query.at("state"));
    EXPECT_EQ(sent["redirect_uri"], "http://localhost:53692/callback");
}

TEST_F(OauthBrowserLoginTest, ABusySharedPortFallsBackToAPastedRedirectUrl) {
    m_log.listenFails = true;
    std::string asked;
    m_interaction.manualCode = [&](const std::string& message) -> std::optional<std::string> {
        asked = message;
        const auto query = queryOf(m_urls[0]);
        return "http://localhost:53692/callback?code=pasted&state=" + query.at("state");
    };
    m_http.enqueue(reply(Json{{"access_token", "tok"}, {"refresh_token", "ref"}, {"expires_in", 60}}));
    const auto credential = login(anthropic(), "anthropic");
    ASSERT_TRUE(credential.has_value()) << credential.error().message;
    EXPECT_NE(asked.find("paste"), std::string::npos);
    EXPECT_EQ(Json::parse(m_http.requests()[0].body)["code"], "pasted");
    EXPECT_EQ(m_log.paths.size(), 0U);
}

TEST_F(OauthBrowserLoginTest, PastedInputMayBeACodeWithStateAQueryOrABareCode) {
    m_interaction.manualOnly = true;
    for (const std::string& input : std::vector<std::string>{"the-code#STATE", "code=the-code&state=STATE", "  the-code  "}) {
        m_urls.clear();
        m_interaction.manualCode = [&](const std::string&) -> std::optional<std::string> {
            const std::string state = queryOf(m_urls[0]).at("state");
            std::string text = input;
            if (const auto at = text.find("STATE"); at != std::string::npos) {
                text.replace(at, 5, state);
            }
            return text;
        };
        m_http.enqueue(reply(Json{{"access_token", "tok"}, {"refresh_token", "ref"}, {"expires_in", 60}}));
        const auto credential = login(anthropic(), "anthropic");
        ASSERT_TRUE(credential.has_value()) << input << ": " << credential.error().message;
        EXPECT_EQ(Json::parse(m_http.requests().back().body)["code"], "the-code");
    }
    EXPECT_EQ(m_log.requestedPort, -1);
}

TEST_F(OauthBrowserLoginTest, AMismatchingStateInAPastedUrlIsRefused) {
    m_interaction.manualOnly = true;
    m_interaction.manualCode = [&](const std::string&) -> std::optional<std::string> { return "http://localhost/cb?code=c&state=other"; };
    const auto credential = login(anthropic(), "anthropic");
    ASSERT_FALSE(credential.has_value());
    EXPECT_EQ(credential.error().message, "OAuth state mismatch");
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(OauthBrowserLoginTest, TheCopyCodeVariantUsesTheProvidersCodePage) {
    m_interaction.manualCode = [&](const std::string&) -> std::optional<std::string> { return "shown-code#" + queryOf(m_urls[0]).at("state"); };
    m_http.enqueue(reply(Json{{"access_token", "tok"}, {"refresh_token", "ref"}, {"expires_in", 60}}));
    const auto credential = login(anthropic(), "anthropic", true);
    ASSERT_TRUE(credential.has_value()) << credential.error().message;
    EXPECT_EQ(queryOf(m_urls[0]).at("redirect_uri"), "https://platform.claude.com/oauth/code/callback");
    EXPECT_EQ(Json::parse(m_http.requests()[0].body)["redirect_uri"], "https://platform.claude.com/oauth/code/callback");
    EXPECT_EQ(m_log.requestedPort, -1);
}

TEST_F(OauthBrowserLoginTest, CodexSendsAFormWithARandomStateAndStoresTheAccountId) {
    BrowserLoginSpec spec;
    spec.authorizeUrl = "https://auth.openai.com/oauth/authorize";
    spec.clientId = "app_EMoamEEZ73f0CkXaXp7hrann";
    spec.scope = "openid profile email offline_access";
    spec.port = 1455;
    spec.path = "/auth/callback";
    spec.pasteWhenPortIsTaken = true;
    spec.authorizeParams = {{"originator", "pi"}};
    spec.providerName = "OpenAI";
    const std::string payload = m_base64.encodeUrl(Json{{"https://api.openai.com/auth", Json{{"chatgpt_account_id", "acct-9"}}}}.dump());
    m_http.enqueue(reply(Json{{"access_token", "h." + payload + ".s"}, {"refresh_token", "ref"}, {"expires_in", 60}}));
    const auto credential = login(spec, "openai-codex");
    ASSERT_TRUE(credential.has_value()) << credential.error().message;
    EXPECT_EQ(credential->extra["accountId"], "acct-9");
    const auto query = queryOf(m_urls[0]);
    EXPECT_EQ(query.at("originator"), "pi");
    EXPECT_EQ(query.at("state").size(), 32U);
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://auth.openai.com/oauth/token");
    EXPECT_NE(request.body.find("grant_type=authorization_code"), std::string::npos);
    EXPECT_NE(request.body.find("redirect_uri=http%3A%2F%2Flocalhost%3A1455%2Fauth%2Fcallback"), std::string::npos);
}

TEST_F(OauthBrowserLoginTest, ChatgptTakesTheIssuedClientIdFromTheCallbackAndNeedsAnIdToken) {
    BrowserLoginSpec spec;
    spec.authorizeUrl = "https://auth.openai.com/api/accounts/authorize";
    spec.clientId = "dynamic_agent_client";
    spec.scope = "openid chatgpt.tokens.use.direct";
    spec.redirectHost = "127.0.0.1";
    spec.port = 1455;
    spec.path = "/auth/callback";
    spec.dynamicClientId = true;
    spec.requireIdToken = true;
    spec.providerName = "ChatGPT";
    m_log.answer = [](const std::string& state) -> Result<Json> { return Json{{"code", "c"}, {"state", state}, {"client_id", "issued-1"}}; };
    m_http.enqueue(reply(Json{{"access_token", "a"}, {"refresh_token", "r"}, {"expires_in", 60}, {"scope", "openid chatgpt.tokens.use.direct"}, {"id_token", "idt"}}));
    const auto credential = login(spec, "openai", false, {{"ext_agent_host_id", "urn:uuid:1234"}, {"nonce", "n1"}});
    ASSERT_TRUE(credential.has_value()) << credential.error().message;
    EXPECT_EQ(credential->extra["clientId"], "issued-1");
    const auto query = queryOf(m_urls[0]);
    EXPECT_EQ(query.at("client_id"), "dynamic_agent_client");
    EXPECT_EQ(query.at("ext_agent_host_id"), "urn:uuid:1234");
    EXPECT_EQ(query.at("redirect_uri"), "http://127.0.0.1:1455/auth/callback");
    EXPECT_NE(m_http.requests()[0].body.find("client_id=issued-1"), std::string::npos);

    m_http.enqueue(reply(Json{{"access_token", "a"}, {"refresh_token", "r"}, {"expires_in", 60}, {"scope", "openid chatgpt.tokens.use.direct"}}));
    const auto noId = login(spec, "openai");
    ASSERT_FALSE(noId.has_value());
    EXPECT_NE(noId.error().message.find("ID token"), std::string::npos);

    m_log.answer = [](const std::string& state) -> Result<Json> { return Json{{"code", "c"}, {"state", state}}; };
    const auto noClient = login(spec, "openai");
    ASSERT_FALSE(noClient.has_value());
    EXPECT_NE(noClient.error().message.find("issued client ID"), std::string::npos);
}

TEST_F(OauthBrowserLoginTest, ChatgptRequiresItsPortAndFailsWithoutAPasteFallback) {
    BrowserLoginSpec spec;
    spec.authorizeUrl = "https://auth.openai.com/api/accounts/authorize";
    spec.port = 1455;
    spec.providerName = "ChatGPT";
    m_log.listenFails = true;
    m_interaction.manualCode = [](const std::string&) -> std::optional<std::string> { return "x"; };
    const auto credential = login(spec, "openai");
    ASSERT_FALSE(credential.has_value());
    EXPECT_EQ(credential.error().code, "callback_server");
    EXPECT_NE(credential.error().message.find("in use"), std::string::npos);
}

TEST_F(OauthBrowserLoginTest, OpenRouterExchangesTheCodeForAKeyWithoutState) {
    BrowserLoginSpec spec;
    spec.authorizeUrl = "https://openrouter.ai/auth";
    spec.redirectHost = "127.0.0.1";
    spec.path = "/oauth/callback";
    spec.randomPath = true;
    spec.noState = true;
    spec.sendClientId = false;
    spec.redirectParam = "callback_url";
    spec.exchangesForKey = true;
    spec.exchangeUrl = "https://openrouter.ai/api/v1/auth/keys";
    spec.providerName = "OpenRouter";
    m_log.answer = [](const std::string&) -> Result<Json> { return Json{{"code", "or-code"}}; };
    m_http.enqueue(reply(Json{{"key", "sk-or-1"}}));
    const auto credential = login(spec, "openrouter");
    ASSERT_TRUE(credential.has_value()) << credential.error().message;
    EXPECT_EQ(credential->access, "sk-or-1");
    EXPECT_EQ(credential->refresh, "");
    EXPECT_DOUBLE_EQ(credential->expires, 9007199254740991.0);
    const auto query = queryOf(m_urls[0]);
    EXPECT_FALSE(query.contains("state"));
    EXPECT_FALSE(query.contains("client_id"));
    EXPECT_TRUE(query.at("callback_url").starts_with("http://127.0.0.1:41234/oauth/callback/"));
    EXPECT_TRUE(m_log.paths[0].starts_with("/oauth/callback/"));
    EXPECT_EQ(m_log.state, "");
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://openrouter.ai/api/v1/auth/keys");
    EXPECT_EQ(Json::parse(request.body)["code"], "or-code");
    EXPECT_EQ(Json::parse(request.body)["code_challenge_method"], "S256");
}

TEST_F(OauthBrowserLoginTest, AuthorizationErrorsAndFailedExchangesAreReported) {
    m_log.answer = [](const std::string&) -> Result<Json> { return Json{{"error", "access_denied"}, {"error_description", "no thanks"}}; };
    auto result = login(anthropic(), "anthropic");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "oauth:access_denied");
    EXPECT_EQ(result.error().message, "Anthropic authorization failed: no thanks");

    m_log.answer = [](const std::string& state) -> Result<Json> { return Json{{"code", "c"}, {"state", state}}; };
    m_http.enqueue(reply(Json{{"error", "invalid_grant"}}, 400));
    result = login(anthropic(), "anthropic");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("token exchange failed (400)"), std::string::npos);

    m_log.answer = [](const std::string&) -> Result<Json> { return std::unexpected(Error{"timeout", "Timed out waiting for the browser"}); };
    result = login(anthropic(), "anthropic");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "timeout");
    EXPECT_EQ(m_log.closes, 3);
}

TEST_F(OauthBrowserLoginTest, WithoutAWayToAskForTheCodeAFailedListenIsAnError) {
    m_log.listenFails = true;
    const auto result = login(anthropic(), "anthropic");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "callback_server");
}
