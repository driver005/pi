#include <gtest/gtest.h>

import std;
import pi.support.settings_resolver;

TEST(SettingsResolverTest, DefaultsApplyWhenNothingIsSet) {
    ResolvedSettings resolved = SettingsResolver().resolve(HarnessRunSettings{});
    EXPECT_TRUE(resolved.retry.enabled);
    EXPECT_EQ(resolved.retry.maxRetries, 3);
    EXPECT_EQ(resolved.retry.baseDelayMs, 2000);
    EXPECT_EQ(resolved.retry.maxAgentDelayMs, 60000);
    EXPECT_EQ(resolved.compaction.reserveTokens, 16384);
    EXPECT_EQ(resolved.compaction.keepRecentTokens, 20000);
    EXPECT_EQ(resolved.compaction.backgroundTokens, 32768);
    EXPECT_EQ(resolved.toolExecution, "parallel");
    EXPECT_EQ(resolved.steeringMode, "one-at-a-time");
    EXPECT_EQ(resolved.followUpMode, "one-at-a-time");
    EXPECT_FALSE(resolved.extensions.has_value());
}

TEST(SettingsResolverTest, PartialPoliciesMergeFieldByField) {
    HarnessRunSettings settings;
    settings.retry = Json::parse(R"({"maxRetries":0,"maxAgentDelayMs":null})");
    settings.compaction = Json::parse(R"({"enabled":false,"backgroundTokens":0})");
    settings.toolExecution = "sequential";
    settings.extensions = std::vector<std::string>{"a", "b"};
    settings.stream = Json::parse(R"({"timeoutMs":5})");
    ResolvedSettings resolved = SettingsResolver().resolve(settings);
    EXPECT_EQ(resolved.retry.maxRetries, 0);
    EXPECT_TRUE(resolved.retry.enabled);
    EXPECT_FALSE(resolved.retry.maxAgentDelayMs.has_value());
    EXPECT_FALSE(resolved.compaction.enabled);
    EXPECT_EQ(resolved.compaction.backgroundTokens, 0);
    EXPECT_EQ(resolved.compaction.reserveTokens, 16384);
    EXPECT_EQ(resolved.toolExecution, "sequential");
    EXPECT_EQ(*resolved.extensions, std::vector<std::string>({"a", "b"}));
    EXPECT_EQ(resolved.stream.at("timeoutMs"), 5);
}
