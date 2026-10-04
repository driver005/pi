#include <gtest/gtest.h>

import std;
import pi.support.resolved_agent;

TEST(ResolvedAgentTest, HooksFollowExtensionOrderAndMatchTheTaskName) {
    auto first = std::make_shared<Extension>();
    first->name = "first";
    first->hooks.push_back(HookRegistration{"pi.generation", {}});
    first->hooks.push_back(HookRegistration{"pi.tool", {}});
    auto second = std::make_shared<Extension>();
    second->name = "second";
    second->hooks.push_back(HookRegistration{"pi.generation", {}});
    ResolvedAgent agent(std::make_shared<AgentSnapshot>(), {}, {first, second});
    EXPECT_EQ(agent.hooks("pi.generation").size(), 2u);
    EXPECT_EQ(agent.hooks("pi.tool").size(), 1u);
    EXPECT_TRUE(agent.hooks("pi.compaction").empty());
    EXPECT_TRUE(agent.sections().empty());
    EXPECT_EQ(agent.snapshot()->thinkingLevel, "off");
}
