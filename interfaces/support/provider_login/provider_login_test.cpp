#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.memory_credential_store;
import pi.base.base64_codec;
import pi.base.boring_crypto;
import pi.support.provider_login;
import pi.testing.fake_environment;
import pi.testing.fixed_clock;
import pi.testing.scripted_http_client;
import pi.testing.scripted_process_runner;

class AdvancingSleeper : public ISleeper {
public:
    explicit AdvancingSleeper(FixedClock& clock)
        : m_clock(clock) {}

    bool sleep(std::chrono::milliseconds duration, const std::shared_ptr<AbortSignal>& signal) override {
        m_clock.advance(duration.count());
        return !(signal && signal->aborted());
    }

private:
    FixedClock& m_clock;
};

class FakeCallbackServer : public ICallbackServer {
public:
    Result<int> listen(const std::string&, int port, bool) override {
        return port != 0 ? port : 40000;
    }

    Result<Json> waitForCallback(const std::vector<std::string>&, const std::string& state, std::chrono::milliseconds, const std::shared_ptr<AbortSignal>&) override {
        return Json{{"code", "auth-code"}, {"state", state}, {"client_id", "issued-1"}};
    }

    void close() override {}
};

/** The Copilot flow's refresh: turns the GitHub token into a Copilot token. */
class FakeCopilotFlow : public IOauthFlow {
public:
    std::string providerId() const override {
        return "github-copilot";
    }

    std::string name() const override {
        return "GitHub Copilot";
    }

    bool isSubscription() const override {
        return true;
    }

    Result<Credential> refresh(const Credential& credential, const std::shared_ptr<AbortSignal>&) override {
        m_seenRefresh = credential.refresh;
        Credential out = credential;
        out.access = "copilot-token";
        out.expires = 5000;
        return out;
    }

    ModelAuth toAuth(const Credential&) const override {
        return {};
    }

    std::string m_seenRefresh;
};

/** The Meta flow's mint: turns the identity token into an API key. */
class FakeMetaFlow : public IOauthFlow {
public:
    std::string providerId() const override {
        return "meta";
    }

    std::string name() const override {
        return "Meta";
    }

    bool isSubscription() const override {
        return true;
    }

    Result<Credential> refresh(const Credential& credential, const std::shared_ptr<AbortSignal>&) override {
        m_seenIdentity = credential.refresh;
        Credential out = credential;
        out.access = "minted-key";
        out.expires = 86'400'000;
        return out;
    }

    ModelAuth toAuth(const Credential&) const override {
        return {};
    }

    std::string m_seenIdentity;
};

class FakePluginOauth : public IPluginOauth {
public:
    std::vector<std::pair<std::string, std::string>> oauthProviders() const override {
        return {{"acme", "Acme"}, {"anthropic", "Not the built-in one"}};
    }

    Result<Credential> oauthLogin(const std::string& provider, const LoginInteraction& interaction) override {
        if (provider != "acme") {
            return std::unexpected(Error{"unknown_provider", "no"});
        }
        if (interaction.authUrl) {
            interaction.authUrl("https://acme.test/login", "go");
        }
        Credential credential;
        credential.type = CredentialType::OAuth;
        credential.access = "acme-access";
        credential.refresh = "acme-refresh";
        credential.expires = 7000;
        return credential;
    }
};

class ProviderLoginTest : public testing::Test {
protected:
    ProviderLoginTest()
        : m_processes([](const ProcessRequest&) -> Result<ProcessResult> { return ProcessResult{}; }),
          m_configValues(m_environment, m_processes),
          m_credentials(m_configValues) {
        m_interaction.authUrl = [this](const std::string& url, const std::string&) { m_urls.push_back(url); };
        m_interaction.deviceCode = [this](const std::string& code, const std::string& uri, std::optional<std::int64_t>, std::optional<std::int64_t>) { m_device = {code, uri}; };
    }

    HttpResponse reply(const Json& body, int status = 200) {
        HttpResponse response;
        response.status = status;
        response.body = body.dump();
        return response;
    }

    Credential stored(const std::string& provider) {
        auto credential = m_credentials.read(provider);
        EXPECT_TRUE(credential.has_value() && credential->has_value());
        return **credential;
    }

    FixedClock m_clock{1'000'000};
    BoringCrypto m_crypto;
    Base64Codec m_base64;
    FakeEnvironment m_environment;
    ScriptedProcessRunner m_processes;
    ConfigValueResolver m_configValues;
    MemoryCredentialStore m_credentials;
    ScriptedHttpClient m_http;
    AdvancingSleeper m_sleeper{m_clock};
    FakeCopilotFlow m_copilot;
    FakeMetaFlow m_meta;
    LoginInteraction m_interaction;
    std::vector<std::string> m_urls;
    std::vector<std::string> m_device;
    ProviderLogin m_login{m_credentials, m_http, m_crypto, m_base64, m_clock, m_sleeper, []() { return std::unique_ptr<ICallbackServer>(std::make_unique<FakeCallbackServer>()); }, {{"github-copilot", &m_copilot}, {"meta", &m_meta}}, "https://kimi.example"};
};

TEST_F(ProviderLoginTest, ListsTheProvidersAndTheirMethods) {
    EXPECT_EQ(m_login.methods("anthropic"), (std::vector<std::string>{"browser", "copy_code"}));
    EXPECT_EQ(m_login.methods("xai"), std::vector<std::string>{"device_code"});
    EXPECT_TRUE(m_login.methods("nobody").empty());
    EXPECT_FALSE(m_login.providers().empty());
}

TEST_F(ProviderLoginTest, UnknownProvidersAndMethodsAreRefused) {
    auto result = m_login.login("nobody", "", m_interaction);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "unknown_provider");
    result = m_login.login("anthropic", "device_code", m_interaction);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "unknown_method");
}

TEST_F(ProviderLoginTest, ABrowserSignInStoresTheCredential) {
    m_http.enqueue(reply(Json{{"access_token", "tok"}, {"refresh_token", "ref"}, {"expires_in", 3600}}));
    ASSERT_TRUE(m_login.login("anthropic", "", m_interaction).has_value());
    const Credential credential = stored("anthropic");
    EXPECT_EQ(credential.type, CredentialType::OAuth);
    EXPECT_EQ(credential.access, "tok");
    EXPECT_EQ(credential.refresh, "ref");
    EXPECT_EQ(m_urls.size(), 1U);
}

TEST_F(ProviderLoginTest, ChatgptSendsAHostIdAndANonce) {
    m_http.enqueue(reply(Json{{"access_token", "a"}, {"refresh_token", "r"}, {"expires_in", 60}, {"scope", "openid chatgpt.tokens.use.direct"}, {"id_token", "idt"}}));
    ASSERT_TRUE(m_login.login("openai", "browser", m_interaction).has_value());
    EXPECT_EQ(stored("openai").extra["clientId"], "issued-1");
    EXPECT_NE(m_urls[0].find("ext_agent_host_id=urn%3Auuid%3A"), std::string::npos);
    EXPECT_NE(m_urls[0].find("&nonce="), std::string::npos);
    const std::string id = m_urls[0].substr(m_urls[0].find("urn%3Auuid%3A") + 13, 36);
    EXPECT_EQ(id[8], '-');
    EXPECT_EQ(id[14], '4');
}

TEST_F(ProviderLoginTest, OpenRouterStoresTheKeyAsALongLivedCredential) {
    m_http.enqueue(reply(Json{{"key", "sk-or-1"}}));
    ASSERT_TRUE(m_login.login("openrouter", "", m_interaction).has_value());
    EXPECT_EQ(stored("openrouter").access, "sk-or-1");
}

TEST_F(ProviderLoginTest, XaiAndKimiSignInWithTheDeviceCode) {
    m_http.enqueue(reply(Json{{"device_code", "d"}, {"user_code", "XAI-1"}, {"verification_uri", "https://x.ai/device"}, {"verification_uri_complete", "https://x.ai/device?c=XAI-1"}, {"expires_in", 600}, {"interval", 1}}));
    m_http.enqueue(reply(Json{{"access_token", "xtok"}, {"refresh_token", "xref"}, {"expires_in", 3600}}));
    ASSERT_TRUE(m_login.login("xai", "", m_interaction).has_value());
    EXPECT_EQ(m_device, (std::vector<std::string>{"XAI-1", "https://x.ai/device?c=XAI-1"}));
    EXPECT_EQ(stored("xai").access, "xtok");
    EXPECT_DOUBLE_EQ(stored("xai").expires, static_cast<double>(m_clock.nowMs()) + 3'600'000 - 300'000);

    m_http.enqueue(reply(Json{{"device_code", "d"}, {"user_code", "KIMI-1"}, {"verification_uri", "https://kimi.example/v"}, {"verification_uri_complete", "https://kimi.example/v?c=KIMI-1"}}));
    m_http.enqueue(reply(Json{{"access_token", "ktok"}, {"refresh_token", "kref"}, {"expires_in", 3600}}));
    ASSERT_TRUE(m_login.login("kimi-coding", "", m_interaction).has_value());
    EXPECT_EQ(m_http.requests()[2].url, "https://kimi.example/api/oauth/device_authorization");
    EXPECT_EQ(stored("kimi-coding").refresh, "kref");
}

TEST_F(ProviderLoginTest, CopilotTradesTheGithubTokenForACopilotToken) {
    m_http.enqueue(reply(Json{{"device_code", "d"}, {"user_code", "GH-1"}, {"verification_uri", "https://github.com/login/device"}, {"expires_in", 900}, {"interval", 5}}));
    m_http.enqueue(reply(Json{{"error", "authorization_pending"}}));
    m_http.enqueue(reply(Json{{"access_token", "gho_abc"}, {"token_type", "bearer"}}));
    ASSERT_TRUE(m_login.login("github-copilot", "", m_interaction).has_value());
    EXPECT_EQ(m_copilot.m_seenRefresh, "gho_abc");
    EXPECT_EQ(stored("github-copilot").access, "copilot-token");
    EXPECT_EQ(m_http.requests()[0].url, "https://github.com/login/device/code");
    bool agent = false;
    const HttpRequest first = m_http.requests()[0];
    for (const auto& [name, value] : first.headers) {
        agent = agent || (name == "User-Agent" && value.starts_with("GitHubCopilotChat"));
    }
    EXPECT_TRUE(agent);
}

TEST_F(ProviderLoginTest, CodexDeviceCodeUsesItsOwnEndpointsAndStoresTheAccountId) {
    m_http.enqueue(reply(Json{{"device_auth_id", "da-1"}, {"user_code", "CODEX-1"}, {"interval", "3"}}));
    m_http.enqueue(reply(Json::object(), 403));
    m_http.enqueue(reply(Json{{"authorization_code", "ac"}, {"code_verifier", "cv"}}));
    const std::string payload = m_base64.encodeUrl(Json{{"https://api.openai.com/auth", Json{{"chatgpt_account_id", "acct-1"}}}}.dump());
    m_http.enqueue(reply(Json{{"access_token", "h." + payload + ".s"}, {"refresh_token", "ref"}, {"expires_in", 60}}));
    ASSERT_TRUE(m_login.login("openai-codex", "device_code", m_interaction).has_value());
    EXPECT_EQ(m_device, (std::vector<std::string>{"CODEX-1", "https://auth.openai.com/codex/device"}));
    const auto requests = m_http.requests();
    ASSERT_EQ(requests.size(), 4U);
    EXPECT_EQ(requests[0].url, "https://auth.openai.com/api/accounts/deviceauth/usercode");
    EXPECT_EQ(Json::parse(requests[1].body)["device_auth_id"], "da-1");
    EXPECT_EQ(requests[3].url, "https://auth.openai.com/oauth/token");
    EXPECT_NE(requests[3].body.find("redirect_uri=https%3A%2F%2Fauth.openai.com%2Fdeviceauth%2Fcallback"), std::string::npos);
    EXPECT_NE(requests[3].body.find("code_verifier=cv"), std::string::npos);
    EXPECT_EQ(stored("openai-codex").extra["accountId"], "acct-1");
}

TEST_F(ProviderLoginTest, CodexDeviceCodeReportsWhatTheServerSays) {
    m_http.enqueue(reply(Json::object(), 404));
    auto result = m_login.login("openai-codex", "device_code", m_interaction);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("not enabled for this server"), std::string::npos);
    m_http.enqueue(reply(Json{{"device_auth_id", "da"}, {"user_code", "u"}, {"interval", 1}}));
    m_http.enqueue(reply(Json{{"error", Json{{"code", "deviceauth_authorization_pending"}}}}, 400));
    m_http.enqueue(reply(Json{{"error", "boom"}}, 500));
    result = m_login.login("openai-codex", "device_code", m_interaction);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("failed with status 500"), std::string::npos);
}

TEST_F(ProviderLoginTest, AFailedSignInStoresNothingAndLogoutForgetsTheCredential) {
    m_http.enqueue(reply(Json{{"error", "nope"}}, 400));
    EXPECT_FALSE(m_login.login("anthropic", "", m_interaction).has_value());
    auto none = m_credentials.read("anthropic");
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->has_value());

    m_http.enqueue(reply(Json{{"access_token", "tok"}, {"refresh_token", "ref"}, {"expires_in", 3600}}));
    ASSERT_TRUE(m_login.login("anthropic", "", m_interaction).has_value());
    ASSERT_TRUE(m_login.logout("anthropic").has_value());
    EXPECT_FALSE((*m_credentials.read("anthropic")).has_value());
}

TEST_F(ProviderLoginTest, PluginProvidersSignInThroughTheirPlugin) {
    FakePluginOauth plugins;
    EXPECT_TRUE(m_login.methods("acme").empty()) << "without plugins nobody signs in to it";
    m_login.setPlugins(&plugins);
    EXPECT_EQ(m_login.methods("acme"), std::vector<std::string>{"plugin"});
    EXPECT_EQ(m_login.methods("anthropic"), (std::vector<std::string>{"browser", "copy_code"})) << "a built-in provider keeps its sign-in";
    const std::vector<std::string> providers = m_login.providers();
    EXPECT_EQ(std::ranges::count(providers, "acme"), 1);
    EXPECT_EQ(std::ranges::count(providers, "anthropic"), 1);

    ASSERT_TRUE(m_login.login("acme", "", m_interaction).has_value());
    EXPECT_EQ(m_urls, std::vector<std::string>{"https://acme.test/login"});
    const Credential credential = stored("acme");
    EXPECT_EQ(credential.access, "acme-access");
    EXPECT_EQ(credential.refresh, "acme-refresh");
    EXPECT_EQ(credential.expires, 7000);

    const auto wrongMethod = m_login.login("acme", "browser", m_interaction);
    ASSERT_FALSE(wrongMethod.has_value());
    EXPECT_EQ(wrongMethod.error().code, "unknown_method");
}

TEST_F(ProviderLoginTest, MetaSignsInWithTheDeviceCodeAndTradesTheIdentityTokenForAKey) {
    EXPECT_EQ(m_login.methods("meta"), std::vector<std::string>{"device_code"});
    m_http.enqueue(reply(Json{{"device_code", "d"}, {"user_code", "META-1"}, {"verification_uri", "https://meta.com/device"}, {"verification_uri_complete", "https://meta.com/device?c=META-1"}, {"expires_in", 600}, {"interval", 1}}));
    m_http.enqueue(reply(Json{{"access_token", "identity-1"}}));
    std::vector<std::string> progress;
    m_interaction.progress = [&progress](const std::string& message) { progress.push_back(message); };
    ASSERT_TRUE(m_login.login("meta", "", m_interaction).has_value());
    EXPECT_EQ(m_device, (std::vector<std::string>{"META-1", "https://meta.com/device?c=META-1"}));
    EXPECT_EQ(m_http.requests()[0].url, "https://auth.meta.com/oidc/device/authorization/");
    EXPECT_EQ(m_http.requests()[1].url, "https://auth.meta.com/oidc/device/token/");
    EXPECT_EQ(m_meta.m_seenIdentity, "identity-1");
    EXPECT_EQ(progress, std::vector<std::string>{"Enabling Meta Model API access..."});
    const Credential credential = stored("meta");
    EXPECT_EQ(credential.access, "minted-key");
    EXPECT_EQ(credential.refresh, "identity-1");
}

TEST_F(ProviderLoginTest, RadiusBrowserSignInDiscoversTheAuthorizationEndpointAtTheGateway) {
    m_login.setRadiusGateway("https://gw.example");
    EXPECT_EQ(m_login.methods("radius"), (std::vector<std::string>{"browser", "device_code"}));
    m_http.enqueue(reply(Json{{"authorizationEndpoint", "https://gw.example/authorize"}}));
    m_http.enqueue(reply(Json{{"access_token", "rtok"}, {"refresh_token", "rref"}, {"expires_in", 3600}, {"scope", "gateway offline_access"}}));
    ASSERT_TRUE(m_login.login("radius", "browser", m_interaction).has_value());
    EXPECT_EQ(m_http.requests()[0].url, "https://gw.example/v1/oauth");
    EXPECT_EQ(m_http.requests()[1].url, "https://gw.example/v1/oauth/token");
    ASSERT_EQ(m_urls.size(), 1U);
    EXPECT_TRUE(m_urls[0].starts_with("https://gw.example/authorize?")) << m_urls[0];
    EXPECT_NE(m_urls[0].find("client_id=pi-gateway"), std::string::npos);
    EXPECT_NE(m_urls[0].find("handoff=url"), std::string::npos);
    EXPECT_NE(m_urls[0].find("redirect_uri=http%3A%2F%2F127.0.0.1%3A1456%2Foauth%2Fcallback"), std::string::npos);
    const Credential credential = stored("radius");
    EXPECT_EQ(credential.access, "rtok");
    EXPECT_EQ(credential.refresh, "rref");
    EXPECT_EQ(credential.extra["scope"], "gateway offline_access");
    EXPECT_DOUBLE_EQ(credential.expires, static_cast<double>(m_clock.nowMs()) + 3'600'000 - 60'000);
}

TEST_F(ProviderLoginTest, RadiusDeviceSignInUsesTheGatewayEndpoints) {
    m_login.setRadiusGateway("https://gw.example");
    m_http.enqueue(reply(Json{{"device_code", "d"}, {"user_code", "RAD-1"}, {"verification_uri", "https://gw.example/device"}, {"expires_in", 600}, {"interval", 1}}));
    m_http.enqueue(reply(Json{{"error", "authorization_pending"}}, 400));
    m_http.enqueue(reply(Json{{"access_token", "dtok"}, {"refresh_token", "dref"}, {"expires_in", 600}}));
    ASSERT_TRUE(m_login.login("radius", "device_code", m_interaction).has_value());
    EXPECT_EQ(m_device, (std::vector<std::string>{"RAD-1", "https://gw.example/device"}));
    EXPECT_EQ(m_http.requests()[0].url, "https://gw.example/v1/oauth/device");
    EXPECT_EQ(m_http.requests()[1].url, "https://gw.example/v1/oauth/token");
    EXPECT_EQ(stored("radius").access, "dtok");
}

TEST_F(ProviderLoginTest, RadiusDiscoveryFailuresAreReported) {
    m_login.setRadiusGateway("https://gw.example");
    m_http.enqueue(reply(Json::object(), 503));
    auto result = m_login.login("radius", "browser", m_interaction);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("Could not load Radius OAuth config from https://gw.example: 503"), std::string::npos);
    m_http.enqueue(reply(Json{{"nope", 1}}));
    result = m_login.login("radius", "browser", m_interaction);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Invalid Radius OAuth config from https://gw.example");
}
