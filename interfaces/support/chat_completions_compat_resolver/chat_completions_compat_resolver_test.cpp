#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.chat_completions_compat_resolver;

class ChatCompletionsCompatResolverTest : public testing::Test {
protected:
    Model model(const std::string& provider, const std::string& baseUrl,
                const std::string& id = "m") {
        Model m;
        m.provider = provider;
        m.baseUrl = baseUrl;
        m.id = id;
        return m;
    }

    ChatCompletionsCompatResolver m_resolver;
};

TEST_F(ChatCompletionsCompatResolverTest, OpenAiDefaults) {
    const auto compat = m_resolver.resolve(model("openai", "https://api.openai.com/v1"));
    EXPECT_TRUE(compat.supportsStore);
    EXPECT_TRUE(compat.supportsDeveloperRole);
    EXPECT_TRUE(compat.supportsReasoningEffort);
    EXPECT_EQ(compat.maxTokensField, "max_completion_tokens");
    EXPECT_EQ(compat.thinkingFormat, "openai");
    EXPECT_FALSE(compat.sendSessionAffinityHeaders);
}

TEST_F(ChatCompletionsCompatResolverTest, DeepSeekUsesMaxTokensAndDeepseekThinking) {
    const auto compat = m_resolver.resolve(model("deepseek", "https://api.deepseek.com"));
    EXPECT_FALSE(compat.supportsStore);
    EXPECT_FALSE(compat.supportsDeveloperRole);
    EXPECT_EQ(compat.maxTokensField, "max_tokens");
    EXPECT_EQ(compat.thinkingFormat, "deepseek");
    EXPECT_TRUE(compat.requiresReasoningContentOnAssistantMessages);
}

TEST_F(ChatCompletionsCompatResolverTest, OpenRouterAnthropicCacheControl) {
    const auto compat =
        m_resolver.resolve(model("openrouter", "https://openrouter.ai/api/v1", "anthropic/claude"));
    EXPECT_EQ(compat.cacheControlFormat, "anthropic");
    EXPECT_TRUE(compat.supportsDeveloperRole);
    EXPECT_EQ(compat.thinkingFormat, "openrouter");
    EXPECT_TRUE(compat.sendSessionAffinityHeaders);
    EXPECT_EQ(compat.sessionAffinityFormat, "openrouter");
    const auto other =
        m_resolver.resolve(model("openrouter", "https://openrouter.ai/api/v1", "meta/llama"));
    EXPECT_FALSE(other.supportsDeveloperRole);
    EXPECT_FALSE(other.cacheControlFormat.has_value());
}

TEST_F(ChatCompletionsCompatResolverTest, ExplicitCompatOverridesDetection) {
    Model m = model("deepseek", "https://api.deepseek.com");
    m.compat = Json::parse(R"({"maxTokensField":"max_completion_tokens","supportsStore":true,"thinkingTokenBudgetField":"budget"})");
    const auto compat = m_resolver.resolve(m);
    EXPECT_EQ(compat.maxTokensField, "max_completion_tokens");
    EXPECT_TRUE(compat.supportsStore);
    EXPECT_EQ(compat.thinkingTokenBudgetField, "budget");
    EXPECT_EQ(compat.thinkingFormat, "deepseek");
}

TEST_F(ChatCompletionsCompatResolverTest, GrokHasNoReasoningEffort) {
    EXPECT_FALSE(m_resolver.resolve(model("xai", "https://api.x.ai/v1")).supportsReasoningEffort);
    EXPECT_FALSE(m_resolver.resolve(model("together", "https://api.together.xyz/v1")).supportsLongCacheRetention);
}
