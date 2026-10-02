#include <gtest/gtest.h>

import std;
import pi.support.builtin_provider_table;

TEST(BuiltinProviderTableTest, ContainsKnownProviders) {
    BuiltinProviderTable table;
    const auto anthropic = table.find("anthropic");
    ASSERT_TRUE(anthropic.has_value());
    EXPECT_EQ(anthropic->baseUrl, "https://api.anthropic.com");
    EXPECT_TRUE(anthropic->supportsOAuth);
    EXPECT_EQ(anthropic->envVars.size(), 3U);
    const auto codex = table.find("openai-codex");
    ASSERT_TRUE(codex.has_value());
    EXPECT_FALSE(codex->supportsApiKey);
    EXPECT_FALSE(table.find("nope").has_value());
}

TEST(BuiltinProviderTableTest, IdsAreUnique) {
    BuiltinProviderTable table;
    std::set<std::string> ids;
    for (const auto& definition : table.all()) {
        EXPECT_TRUE(ids.insert(definition.id).second) << definition.id;
    }
}

TEST(BuiltinProviderTableTest, EnvVarsMatchEnvKeyTable) {
    // The envKeyTable and this table must agree; spot check two entries.
    BuiltinProviderTable table;
    EXPECT_EQ(table.find("openai")->envVars, (std::vector<std::string>{"OPENAI_API_KEY"}));
    EXPECT_EQ(table.find("google")->envVars, (std::vector<std::string>{"GEMINI_API_KEY"}));
}
