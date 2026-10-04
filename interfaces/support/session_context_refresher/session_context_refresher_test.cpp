#include <gtest/gtest.h>

import std;
import pi.support.session_context_refresher;
import pi.testing.session_harness;

TEST(SessionContextRefresherTest, AgentFollowsTheSessionProjection) {
    SessionHarness harness;
    auto agent = harness.makeAgent();
    SessionContextRefresher refresher(*agent, harness.session());
    ASSERT_EQ(agent->messages().size(), 1U);  // the agent's own system prompt
    UserMessage user;
    user.content = std::string("hello");
    ASSERT_TRUE(harness.session().appendMessage(user).has_value());
    refresher.refresh();
    const auto messages = agent->messages();
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<UserMessage>(messages[0]));
}
