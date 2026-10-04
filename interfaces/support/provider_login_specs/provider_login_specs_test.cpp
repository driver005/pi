#include <gtest/gtest.h>

import std;
import pi.support.builtin_oauth_specs;
import pi.support.provider_login_specs;

TEST(ProviderLoginSpecsTest, EveryProviderHasItsMethodsAndTheirDescriptions) {
    const ProviderLoginSpecs specs;
    for (const std::string& provider : specs.providers()) {
        const auto methods = specs.methods(provider);
        ASSERT_FALSE(methods.empty()) << provider;
        for (const std::string& method : methods) {
            if (method == "device_code" && provider != "openai-codex") {
                EXPECT_TRUE(specs.device(provider).has_value()) << provider;
            } else {
                EXPECT_TRUE(specs.browser(provider).has_value()) << provider << " " << method;
            }
        }
    }
    EXPECT_TRUE(specs.methods("nobody").empty());
    EXPECT_FALSE(specs.browser("xai").has_value());
    EXPECT_FALSE(specs.device("anthropic").has_value());
}

TEST(ProviderLoginSpecsTest, TheClientsAgreeWithTheRefreshSpecs) {
    const ProviderLoginSpecs specs;
    for (const OauthRefreshSpec& refresh : BuiltinOauthSpecs().all("https://kimi.example")) {
        if (refresh.clientId.empty()) {
            continue;
        }
        if (const auto browser = specs.browser(refresh.providerId)) {
            EXPECT_EQ(browser->clientId, refresh.clientId) << refresh.providerId;
        }
        if (const auto device = specs.device(refresh.providerId, "https://kimi.example")) {
            EXPECT_EQ(device->clientId, refresh.clientId) << refresh.providerId;
            EXPECT_EQ(device->tokenUrl, refresh.tokenUrl) << refresh.providerId;
        }
    }
}

TEST(ProviderLoginSpecsTest, TheKimiHostIsConfigurable) {
    const auto device = ProviderLoginSpecs().device("kimi-coding", "https://kimi.example/");
    ASSERT_TRUE(device.has_value());
    EXPECT_EQ(device->deviceUrl, "https://kimi.example/api/oauth/device_authorization");
}
