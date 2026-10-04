#include <gtest/gtest.h>

import std;
import pi.support.auth_guidance;

TEST(AuthGuidanceTest, NamesTheProviderOrFallsBack) {
    const AuthGuidance guidance;
    EXPECT_NE(guidance.noApiKeyFound("anthropic").find("anthropic"), std::string::npos);
    EXPECT_NE(guidance.noApiKeyFound("unknown").find("the selected model"), std::string::npos);
    EXPECT_NE(guidance.authenticationFailed("codex").find("\"codex\""), std::string::npos);
    EXPECT_FALSE(guidance.noModelSelected().empty());
    EXPECT_FALSE(guidance.noModelsAvailable().empty());
}
