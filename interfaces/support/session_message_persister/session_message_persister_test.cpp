#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.session_message_persister;
import pi.testing.session_harness;

class SessionMessagePersisterTest : public testing::Test {
protected:
    SessionHarness m_harness;
    SessionMessagePersister m_persister{m_harness.session()};
};

TEST_F(SessionMessagePersisterTest, LlmMessagesBecomeMessageEntries) {
    UserMessage user;
    user.content = std::string("hi");
    const auto id = m_persister.persist(AgentMessage(user));
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(m_harness.session().entry(*id)->type, "message");
}

TEST_F(SessionMessagePersisterTest, CustomMessagesBecomeCustomMessageEntries) {
    CustomMessage custom;
    custom.role = "custom";
    custom.data = Json{{"customType", "note"}, {"content", "text"}, {"display", false}, {"details", Json{{"k", 1}}}};
    const auto id = m_persister.persist(AgentMessage(custom));
    ASSERT_TRUE(id.has_value());
    const auto entry = m_harness.session().entry(*id);
    EXPECT_EQ(entry->type, "custom_message");
    EXPECT_EQ(entry->body["customType"], "note");
    EXPECT_EQ(entry->body["display"], false);
    EXPECT_EQ(entry->body["details"]["k"], 1);
}

TEST_F(SessionMessagePersisterTest, OtherApplicationMessagesAreLeftToTheirOwners) {
    CustomMessage bash;
    bash.role = "bashExecution";
    EXPECT_FALSE(m_persister.persist(AgentMessage(bash)).has_value());
    EXPECT_EQ(m_harness.session().entryCount(), 0U);
}
