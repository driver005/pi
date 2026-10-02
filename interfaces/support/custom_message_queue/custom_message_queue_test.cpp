#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.custom_message_queue;
import pi.testing.recording_session_sink;
import pi.testing.session_harness;

class CustomMessageQueueTest : public testing::Test {
protected:
    CustomMessageQueueTest() : m_agent(m_harness.makeAgent()), m_refresher(*m_agent, m_harness.session()),
          m_queue(m_harness.session(), m_refresher, m_sink) {}

    CustomMessage note(const std::string& text) {
        CustomMessage message;
        message.role = "custom";
        message.data = Json{{"customType", "note"}, {"content", text}, {"display", true}};
        message.timestamp = 5;
        return message;
    }

    SessionHarness m_harness;
    std::unique_ptr<IAgent> m_agent;
    SessionContextRefresher m_refresher;
    RecordingSessionSink m_sink;
    CustomMessageQueue m_queue;
};

TEST_F(CustomMessageQueueTest, AppendNowStoresReloadsAndAnnounces) {
    m_queue.appendNow(note("hello"));
    EXPECT_EQ(m_harness.session().entryCount(), 1U);
    EXPECT_EQ(m_agent->messages().size(), 1U);
    const auto events = m_sink.eventsOf(SessionEventType::Agent);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].agent->type, AgentEventType::MessageStart);
    EXPECT_EQ(events[1].agent->type, AgentEventType::MessageEnd);
}

TEST_F(CustomMessageQueueTest, DeferredMessagesWaitForFlush) {
    m_queue.defer(note("one"));
    m_queue.defer(note("two"));
    EXPECT_TRUE(m_queue.hasDeferred());
    EXPECT_EQ(m_harness.session().entryCount(), 0U);
    EXPECT_TRUE(m_sink.events().empty());
    m_queue.flush();
    EXPECT_FALSE(m_queue.hasDeferred());
    EXPECT_EQ(m_harness.session().entryCount(), 2U);
    EXPECT_EQ(m_sink.eventsOf(SessionEventType::Agent).size(), 4U);
}

TEST_F(CustomMessageQueueTest, NextTurnMessagesAreTakenOnce) {
    m_queue.queueForNextTurn(note("aside"));
    EXPECT_EQ(m_queue.takeNextTurn().size(), 1U);
    EXPECT_TRUE(m_queue.takeNextTurn().empty());
    EXPECT_EQ(m_harness.session().entryCount(), 0U);
}
