#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.memory_credential_store;
import pi.support.provider_auth_resolver;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.scripted_process_runner;
import pi.testing.stub_oauth_flow;

class ProviderAuthResolverTest : public testing::Test {
protected:
    ProviderAuthResolverTest()
        : m_processes([](const ProcessRequest&) -> Result<ProcessResult> {
              ProcessResult result;
              result.exitCode = 0;
              result.output = "from-cmd\n";
              return result;
          }),
          m_config(m_environment, m_processes),
          m_store(m_config),
          m_keys(m_environment, m_files) {
        m_flows["anthropic"] = &m_flow;
        m_resolver = std::make_unique<ProviderAuthResolver>(m_store, m_keys, m_config, m_clock, m_flows);
    }

    ProviderDefinition anthropic() {
        return ProviderDefinition{"anthropic", "Anthropic", "https://api.anthropic.com",
                                  {"ANTHROPIC_AUTH_TOKEN", "ANTHROPIC_OAUTH_TOKEN", "ANTHROPIC_API_KEY"},
                                  true, true, true};
    }

    ProviderDefinition openai() {
        return ProviderDefinition{"openai", "OpenAI", "https://api.openai.com/v1", {"OPENAI_API_KEY"}, true, false, true};
    }

    ProviderDefinition custom() {
        return ProviderDefinition{"ollama", "Ollama", "", {}, true, false};
    }

    void store(const std::string& provider, Credential credential) {
        m_store.modify(provider, [&](const auto&) { return Result<std::optional<Credential>>(std::optional<Credential>(credential)); });
    }

    Credential apiKeyCredential(const std::string& key) {
        Credential credential;
        credential.key = key;
        return credential;
    }

    Credential oauthCredential(double expires) {
        Credential credential;
        credential.type = CredentialType::OAuth;
        credential.access = "old-access";
        credential.refresh = "r";
        credential.expires = expires;
        return credential;
    }

    FakeEnvironment m_environment;
    FakeFileSystem m_files;
    FixedClock m_clock{1000000};
    ScriptedProcessRunner m_processes;
    ConfigValueResolver m_config;
    MemoryCredentialStore m_store;
    EnvKeyTable m_keys;
    StubOauthFlow m_flow;
    std::map<std::string, IOauthFlow*> m_flows;
    std::unique_ptr<ProviderAuthResolver> m_resolver;
};

TEST_F(ProviderAuthResolverTest, NothingConfiguredResolvesToNullopt) {
    const auto result = m_resolver->resolve(openai(), Json(), std::nullopt, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

TEST_F(ProviderAuthResolverTest, EnvironmentVariableProvidesKey) {
    m_environment.set("OPENAI_API_KEY", "sk-env");
    const auto result = m_resolver->resolve(openai(), Json(), std::nullopt, {});
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((*result)->auth.apiKey, "sk-env");
    EXPECT_EQ((*result)->source, "OPENAI_API_KEY");
}

TEST_F(ProviderAuthResolverTest, StoredCredentialWinsOverEnvironment) {
    m_environment.set("OPENAI_API_KEY", "sk-env");
    store("openai", apiKeyCredential("sk-stored"));
    const auto result = m_resolver->resolve(openai(), Json(), std::nullopt, {});
    EXPECT_EQ((*result)->auth.apiKey, "sk-stored");
    EXPECT_EQ((*result)->source, "stored credential");
}

TEST_F(ProviderAuthResolverTest, ApiKeyOverrideBeatsStoredCredential) {
    store("openai", apiKeyCredential("sk-stored"));
    const auto result = m_resolver->resolve(openai(), Json(), "sk-cli", {});
    EXPECT_EQ((*result)->auth.apiKey, "sk-cli");
}

TEST_F(ProviderAuthResolverTest, AnthropicAuthTokenBecomesBearerHeader) {
    m_environment.set("ANTHROPIC_AUTH_TOKEN", "tok");
    const auto result = m_resolver->resolve(anthropic(), Json(), std::nullopt, {});
    ASSERT_TRUE(result->has_value());
    EXPECT_FALSE((*result)->auth.apiKey.has_value());
    ASSERT_EQ((*result)->auth.headers.size(), 1U);
    EXPECT_EQ((*result)->auth.headers[0].second, "Bearer tok");
}

TEST_F(ProviderAuthResolverTest, ModelsJsonApiKeyAndHeadersForCustomProvider) {
    m_environment.set("OLLAMA_TOKEN", "t0k");
    const Json config = Json::parse(R"({"apiKey":"$OLLAMA_TOKEN","headers":{"x-team":"core","x-env":"${OLLAMA_TOKEN}"},"authHeader":true})");
    const auto result = m_resolver->resolve(custom(), config, std::nullopt, {});
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((*result)->auth.apiKey, "t0k");
    EXPECT_EQ((*result)->source, "configured API key");
    HeaderMerger headers;
    EXPECT_EQ(headers.find((*result)->auth.headers, "x-team"), "core");
    EXPECT_EQ(headers.find((*result)->auth.headers, "x-env"), "t0k");
    EXPECT_EQ(headers.find((*result)->auth.headers, "authorization"), "Bearer t0k");
}

TEST_F(ProviderAuthResolverTest, ModelsJsonCommandKey) {
    const auto result = m_resolver->resolve(custom(), Json::parse(R"({"apiKey":"!pass show x"})"), std::nullopt, {});
    EXPECT_EQ((*result)->auth.apiKey, "from-cmd");
}

TEST_F(ProviderAuthResolverTest, MissingEnvReferenceIsAnError) {
    const auto result = m_resolver->resolve(custom(), Json::parse(R"({"apiKey":"$NOPE"})"), std::nullopt, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Failed to resolve API key for provider \"ollama\" from environment variable: NOPE");
}

TEST_F(ProviderAuthResolverTest, CustomProviderWithoutKeyHasNoAuth) {
    const auto result = m_resolver->resolve(custom(), Json::parse(R"({"baseUrl":"http://x"})"), std::nullopt, {});
    EXPECT_FALSE(result->has_value());
}

TEST_F(ProviderAuthResolverTest, AuthHeaderWithoutKeyIsAnError) {
    m_environment.set("ANTHROPIC_AUTH_TOKEN", "tok");
    const auto result = m_resolver->resolve(anthropic(), Json::parse(R"({"authHeader":true})"), std::nullopt, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "authHeader requires a resolved API key");
}

TEST_F(ProviderAuthResolverTest, ValidOAuthCredentialUsedWithoutRefresh) {
    store("anthropic", oauthCredential(2e12));
    const auto result = m_resolver->resolve(anthropic(), Json(), std::nullopt, {});
    EXPECT_EQ((*result)->auth.apiKey, "old-access");
    EXPECT_EQ((*result)->source, "OAuth");
    EXPECT_EQ(m_flow.refreshCount(), 0);
}

TEST_F(ProviderAuthResolverTest, ExpiringOAuthCredentialIsRefreshedAndPersisted) {
    store("anthropic", oauthCredential(1000000 + 60000));
    const auto result = m_resolver->resolve(anthropic(), Json(), std::nullopt, {});
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((*result)->auth.apiKey, "fresh-access");
    EXPECT_EQ(m_flow.refreshCount(), 1);
    EXPECT_EQ((*m_store.read("anthropic"))->access, "fresh-access");
    // Now valid: no second refresh.
    ASSERT_TRUE(m_resolver->resolve(anthropic(), Json(), std::nullopt, {}).has_value());
    EXPECT_EQ(m_flow.refreshCount(), 1);
}

TEST_F(ProviderAuthResolverTest, FailedRefreshIsAnErrorWithoutEnvFallback) {
    m_environment.set("ANTHROPIC_API_KEY", "sk");
    store("anthropic", oauthCredential(1));
    m_flow.failRefreshWith("invalid_grant");
    const auto result = m_resolver->resolve(anthropic(), Json(), std::nullopt, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "OAuth refresh failed for anthropic: invalid_grant");
}

TEST_F(ProviderAuthResolverTest, StoredOAuthWithoutFlowResolvesToNothing) {
    m_environment.set("OPENAI_API_KEY", "sk-env");
    store("openai", oauthCredential(2e12));
    const auto result = m_resolver->resolve(openai(), Json(), std::nullopt, {});
    EXPECT_FALSE(result->has_value());
}

TEST_F(ProviderAuthResolverTest, ProviderScopedEnvOverlaysProcessEnv) {
    const auto result = m_resolver->resolve(openai(), Json(), std::nullopt, {{"OPENAI_API_KEY", "sk-scoped"}});
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((*result)->auth.apiKey, "sk-scoped");
}
