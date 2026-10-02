#include <gtest/gtest.h>

import std;
import pi.session.session_event_hub;

class SessionEventHubTest : public testing::Test {
protected:
    AgentSessionEvent event(SessionEventType type) {
        AgentSessionEvent out;
        out.type = type;
        return out;
    }

    SessionEventHub m_hub;
};

TEST_F(SessionEventHubTest, DeliversInSubscriptionOrder) {
    std::vector<std::string> seen;
    m_hub.subscribe([&](const AgentSessionEvent&) { seen.push_back("first"); });
    m_hub.subscribe([&](const AgentSessionEvent&) { seen.push_back("second"); });
    m_hub.emit(event(SessionEventType::AgentSettled));
    EXPECT_EQ(seen, (std::vector<std::string>{"first", "second"}));
}

TEST_F(SessionEventHubTest, UnsubscribeStopsDelivery) {
    int count = 0;
    const auto id = m_hub.subscribe([&](const AgentSessionEvent&) { ++count; });
    m_hub.emit(event(SessionEventType::QueueUpdate));
    m_hub.unsubscribe(id);
    m_hub.emit(event(SessionEventType::QueueUpdate));
    EXPECT_EQ(count, 1);
}

TEST_F(SessionEventHubTest, ListenerMayUnsubscribeItselfWhileHandling) {
    int count = 0;
    SessionEventHub::ListenerId id = 0;
    id = m_hub.subscribe([&](const AgentSessionEvent&) {
        ++count;
        m_hub.unsubscribe(id);
    });
    m_hub.emit(event(SessionEventType::AgentSettled));
    m_hub.emit(event(SessionEventType::AgentSettled));
    EXPECT_EQ(count, 1);
}

TEST_F(SessionEventHubTest, ClearRemovesEveryone) {
    int count = 0;
    m_hub.subscribe([&](const AgentSessionEvent&) { ++count; });
    m_hub.clear();
    m_hub.emit(event(SessionEventType::AgentSettled));
    EXPECT_EQ(count, 0);
}
