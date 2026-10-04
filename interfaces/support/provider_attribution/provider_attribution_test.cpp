#include <gtest/gtest.h>

import std;
import pi.support.provider_attribution;

class ProviderAttributionTest : public testing::Test {
protected:
    Model model(const std::string& provider, const std::string& baseUrl) {
        Model out;
        out.provider = provider;
        out.baseUrl = baseUrl;
        return out;
    }

    std::optional<std::string> value(const ProviderAttribution::Headers& headers, const std::string& name) {
        for (const auto& header : headers) {
            if (header.first == name) {
                return header.second;
            }
        }
        return std::nullopt;
    }

    ProviderAttribution m_attribution;
};

TEST_F(ProviderAttributionTest, OpenRouterNvidiaAndCloudflareGetAttributionWhileTelemetryIsOn) {
    const auto openrouter = m_attribution.merge(model("openrouter", "https://openrouter.ai/api/v1"), true, std::nullopt, {});
    EXPECT_EQ(value(openrouter, "HTTP-Referer"), "https://pi.dev");
    EXPECT_EQ(value(openrouter, "X-OpenRouter-Title"), "pi");
    EXPECT_EQ(value(m_attribution.merge(model("custom", "https://integrate.api.nvidia.com/v1"), true, std::nullopt, {}), "X-BILLING-INVOKE-ORIGIN"), "Pi");
    EXPECT_EQ(value(m_attribution.merge(model("x", "https://gateway.ai.cloudflare.com/v1/a/b"), true, std::nullopt, {}), "User-Agent"), "pi-coding-agent");
    EXPECT_TRUE(m_attribution.merge(model("openrouter", "https://openrouter.ai/api/v1"), false, std::nullopt, {}).empty());
    EXPECT_TRUE(m_attribution.merge(model("anthropic", "https://api.anthropic.com"), true, std::nullopt, {}).empty());
    EXPECT_TRUE(m_attribution.merge(model("x", "not a url"), true, std::nullopt, {}).empty());
}

TEST_F(ProviderAttributionTest, OpencodeGetsSessionHeadersEvenWithoutTelemetry) {
    const auto headers = m_attribution.merge(model("opencode", "https://opencode.ai/zen"), false, "s-1", {});
    EXPECT_EQ(value(headers, "x-opencode-session"), "s-1");
    EXPECT_EQ(value(headers, "x-opencode-client"), "pi");
    EXPECT_TRUE(m_attribution.merge(model("opencode", "https://opencode.ai/zen"), false, std::nullopt, {}).empty());
    EXPECT_TRUE(m_attribution.merge(model("other", "https://example.com"), false, "s-1", {}).empty());
}

TEST_F(ProviderAttributionTest, CallerHeadersWinAndCanRemove) {
    const ProviderAttribution::Headers caller{{"X-OpenRouter-Title", "mine"}, {"HTTP-Referer", std::nullopt}, {"Extra", "1"}};
    const auto headers = m_attribution.merge(model("openrouter", "https://openrouter.ai/api/v1"), true, std::nullopt, {caller});
    EXPECT_EQ(value(headers, "X-OpenRouter-Title"), "mine");
    EXPECT_EQ(value(headers, "HTTP-Referer"), std::nullopt);
    EXPECT_EQ(value(headers, "Extra"), "1");
    EXPECT_EQ(value(headers, "X-OpenRouter-Categories"), "cli-agent");
}
