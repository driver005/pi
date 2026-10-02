#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.settings_view;

TEST(SettingsViewTest, DefaultsForEmptySettings) {
    SettingsView view;
    EXPECT_FALSE(view.defaultProvider().has_value());
    EXPECT_EQ(view.steeringMode(), "one-at-a-time");
    EXPECT_EQ(view.followUpMode(), "one-at-a-time");
    EXPECT_EQ(view.transport(), "auto");
    EXPECT_TRUE(view.compactionEnabled());
    EXPECT_EQ(view.compactionReserveTokens("p", "m"), 16384);
    EXPECT_EQ(view.compactionKeepRecentTokens("p", "m"), 20000);
    EXPECT_EQ(view.branchSummaryReserveTokens(), 16384);
    EXPECT_TRUE(view.retryEnabled());
    EXPECT_EQ(view.retryMaxRetries(), 3);
    EXPECT_EQ(view.retryBaseDelayMs(), 2000);
    EXPECT_EQ(view.providerMaxRetryDelayMs(), 60000);
    EXPECT_EQ(view.httpIdleTimeoutMs(), 300000);
    EXPECT_EQ(view.cacheWarmingMode(), "streaming");
    EXPECT_EQ(view.defaultProjectTrust(), "ask");
    EXPECT_TRUE(view.enableSkillCommands());
    EXPECT_TRUE(view.imageAutoResize());
    EXPECT_FALSE(view.blockImages());
    EXPECT_FALSE(view.defaultTools().has_value());
    EXPECT_FALSE(view.enabledModels().has_value());
}

TEST(SettingsViewTest, ReadsConfiguredValues) {
    SettingsView view(Json::parse(R"({
        "defaultProvider":"anthropic","defaultModel":"claude","defaultThinkingLevel":"high",
        "modelThinkingLevels":{"anthropic/claude":"low"},"steeringMode":"all","transport":"sse",
        "compaction":{"enabled":false,"reserveTokens":1000,"modelOverrides":{"anthropic/claude":{"reserveTokens":5,"keepRecentTokens":-3}}},
        "retry":{"enabled":false,"maxRetries":9,"provider":{"timeoutMs":5000,"maxRetries":2,"maxRetryDelayMs":10}},
        "httpIdleTimeoutMs":0,"websocketConnectTimeoutMs":1500,"cacheWarming":"idle","defaultProjectTrust":"always",
        "skills":["/s"],"extensions":["/e"],"prompts":["/p"],"defaultTools":["+grep"],"enabledModels":["a","b"],
        "images":{"autoResize":false,"blockImages":true},"thinkingBudgets":{"high":9}})"));
    EXPECT_EQ(view.defaultProvider(), "anthropic");
    EXPECT_EQ(view.modelThinkingLevel("anthropic", "claude"), "low");
    EXPECT_EQ(view.steeringMode(), "all");
    EXPECT_EQ(view.transport(), "sse");
    EXPECT_FALSE(view.compactionEnabled());
    EXPECT_EQ(view.compactionReserveTokens("anthropic", "claude"), 5);
    EXPECT_EQ(view.compactionReserveTokens("other", "m"), 1000);
    // A negative override is invalid and falls back to the default.
    EXPECT_EQ(view.compactionKeepRecentTokens("anthropic", "claude"), 20000);
    EXPECT_FALSE(view.retryEnabled());
    EXPECT_EQ(view.retryMaxRetries(), 9);
    EXPECT_EQ(view.providerTimeoutMs(), 5000);
    EXPECT_EQ(view.providerMaxRetries(), 2);
    EXPECT_EQ(view.providerMaxRetryDelayMs(), 10);
    EXPECT_EQ(view.httpIdleTimeoutMs(), 0);
    EXPECT_EQ(view.websocketConnectTimeoutMs(), 1500);
    EXPECT_EQ(view.cacheWarmingMode(), "idle");
    EXPECT_EQ(view.defaultProjectTrust(), "always");
    EXPECT_EQ(view.skillPaths(), (std::vector<std::string>{"/s"}));
    EXPECT_EQ(view.extensionPaths(), (std::vector<std::string>{"/e"}));
    EXPECT_EQ(view.promptTemplatePaths(), (std::vector<std::string>{"/p"}));
    EXPECT_EQ(*view.defaultTools(), (std::vector<std::string>{"read", "bash", "edit", "write", "grep"}));
    EXPECT_EQ(*view.enabledModels(), (std::vector<std::string>{"a", "b"}));
    EXPECT_FALSE(view.imageAutoResize());
    EXPECT_TRUE(view.blockImages());
    EXPECT_EQ(view.thinkingBudgets()["high"], 9);
}

TEST(SettingsViewTest, WrongTypesFallBack) {
    SettingsView view(Json::parse(R"({"steeringMode":5,"compaction":"x","retry":{"maxRetries":"many"},
        "cacheWarming":"sometimes","defaultProjectTrust":"maybe","skills":"nope","httpIdleTimeoutMs":-1})"));
    EXPECT_EQ(view.steeringMode(), "one-at-a-time");
    EXPECT_TRUE(view.compactionEnabled());
    EXPECT_EQ(view.retryMaxRetries(), 3);
    EXPECT_EQ(view.cacheWarmingMode(), "streaming");
    EXPECT_EQ(view.defaultProjectTrust(), "ask");
    EXPECT_TRUE(view.skillPaths().empty());
    EXPECT_EQ(view.httpIdleTimeoutMs(), 300000);
}
