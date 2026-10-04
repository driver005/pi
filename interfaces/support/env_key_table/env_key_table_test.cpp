#include <gtest/gtest.h>

import std;
import pi.support.env_key_table;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;

class EnvKeyTableTest : public testing::Test {
protected:
    FakeEnvironment m_environment;
    FakeFileSystem m_files{"/home/u"};
    EnvKeyTable m_table{m_environment, m_files};
};

TEST_F(EnvKeyTableTest, KnownProvidersMapToVariables) {
    EXPECT_EQ(m_table.envVars("openai"), (std::vector<std::string>{"OPENAI_API_KEY"}));
    EXPECT_EQ(m_table.envVars("anthropic").size(), 3U);
    EXPECT_TRUE(m_table.envVars("my-custom").empty());
}

TEST_F(EnvKeyTableTest, ApiKeyFromEnvironment) {
    EXPECT_FALSE(m_table.apiKey("openai", {}).has_value());
    m_environment.set("OPENAI_API_KEY", "sk-env");
    EXPECT_EQ(m_table.apiKey("openai", {}), "sk-env");
    EXPECT_EQ(m_table.apiKey("openai", {{"OPENAI_API_KEY", "sk-override"}}), "sk-override");
    m_environment.set("GROQ_API_KEY", "   ");
    EXPECT_FALSE(m_table.apiKey("groq", {}).has_value());
}

TEST_F(EnvKeyTableTest, AnthropicAuthTokenIsNotAnApiKey) {
    m_environment.set("ANTHROPIC_AUTH_TOKEN", "tok");
    EXPECT_EQ(m_table.findEnvKeys("anthropic", {}), (std::vector<std::string>{"ANTHROPIC_AUTH_TOKEN"}));
    EXPECT_FALSE(m_table.apiKey("anthropic", {}).has_value());
    m_environment.set("ANTHROPIC_API_KEY", "key");
    EXPECT_EQ(m_table.apiKey("anthropic", {}), "key");
}

TEST_F(EnvKeyTableTest, VertexNeedsAdcProjectAndLocation) {
    m_environment.set("GOOGLE_CLOUD_PROJECT", "p");
    m_environment.set("GOOGLE_CLOUD_LOCATION", "us-central1");
    EXPECT_FALSE(m_table.apiKey("google-vertex", {}).has_value());
    m_files.createDirectories("/home/u/.config/gcloud");
    m_files.writeFile("/home/u/.config/gcloud/application_default_credentials.json", "{}");
    EXPECT_EQ(m_table.apiKey("google-vertex", {}), "<authenticated>");
}

TEST_F(EnvKeyTableTest, BedrockAmbientCredentials) {
    EXPECT_FALSE(m_table.apiKey("amazon-bedrock", {}).has_value());
    m_environment.set("AWS_ACCESS_KEY_ID", "a");
    EXPECT_FALSE(m_table.apiKey("amazon-bedrock", {}).has_value());
    m_environment.set("AWS_SECRET_ACCESS_KEY", "b");
    EXPECT_EQ(m_table.apiKey("amazon-bedrock", {}), "<authenticated>");
}
