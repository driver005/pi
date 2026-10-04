#include <gtest/gtest.h>

import std;
import pi.agent_factory;
import pi.agent.agent_loop;
import pi.agent.tool_call_runner;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;

TEST(AgentFactoryTest, CreatesIndependentAgents) {
    InlineExecutor executor;
    FixedClock clock;
    ToolCallRunner runner;
    AgentLoop loop(runner, executor, clock);
    AgentFactory factory(loop, clock);
    AgentOptions options;
    options.model.id = "m";
    options.systemPrompt = "prompt";
    auto first = factory.create(options);
    auto second = factory.create(options);
    ASSERT_NE(first, nullptr);
    EXPECT_NE(first.get(), second.get());
    EXPECT_EQ(first->state().systemPrompt, "prompt");
    first->setThinkingLevel(ThinkingLevel::High);
    EXPECT_EQ(second->state().thinkingLevel, ThinkingLevel::Off);
}
