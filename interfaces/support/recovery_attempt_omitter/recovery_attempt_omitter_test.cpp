#include <gtest/gtest.h>

import std;
import pi.support.recovery_attempt_omitter;
import pi.testing.recording_session_sink;
import pi.testing.session_harness;

class RecoveryAttemptOmitterTest : public testing::Test {
protected:
    RecoveryAttemptOmitterTest()
        : m_agent(m_harness.makeAgent()),
          m_refresher(*m_agent, m_harness.session()),
          m_omitter(m_harness.session(), m_events, m_refresher) {}

    AssistantMessage assistant(const std::string& text) {
        AssistantMessage message;
        message.content.push_back(TextContent{text, std::nullopt});
        message.stopReason = StopReason::Error;
        message.errorMessage = "overloaded";
        message.timestamp = 7;
        return message;
    }

    SessionHarness m_harness;
    std::unique_ptr<IAgent> m_agent;
    SessionContextRefresher m_refresher;
    RecordingSessionSink m_events;
    RecoveryAttemptOmitter m_omitter;
};

TEST_F(RecoveryAttemptOmitterTest, OmittedAttemptLeavesContextButStaysInFile) {
    ISessionManager& session = m_harness.session();
    UserMessage user;
    user.content = std::string("question");
    ASSERT_TRUE(session.appendMessage(user).has_value());
    const AssistantMessage failed = assistant("partial");
    ASSERT_TRUE(session.appendMessage(failed).has_value());
    m_refresher.refresh();
    ASSERT_EQ(m_agent->state().messages.size(), 2U);

    ASSERT_TRUE(m_omitter.omit(failed, {}).has_value());

    EXPECT_EQ(m_agent->state().messages.size(), 1U);
    EXPECT_EQ(session.entryCount(), 3U);
    const auto appended = m_events.eventsOf(SessionEventType::EntryAppended);
    ASSERT_EQ(appended.size(), 1U);
    EXPECT_EQ(appended[0].entry->type, "context_edit");
}

TEST_F(RecoveryAttemptOmitterTest, UnknownMessageIsAnError) {
    EXPECT_FALSE(m_omitter.omit(assistant("never stored"), {}).has_value());
    EXPECT_TRUE(m_events.events().empty());
}
