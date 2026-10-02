#include <gtest/gtest.h>

import std;
import pi.support.pending_input_tracker;
import pi.testing.recording_session_sink;

class PendingInputTrackerTest : public testing::Test {
protected:
    RecordingSessionSink m_sink;
    PendingInputTracker m_tracker{m_sink};
};

TEST_F(PendingInputTrackerTest, QueueingAnnouncesEveryChange) {
    m_tracker.queueSteering("stop");
    m_tracker.queueFollowUp("then this");
    const auto updates = m_sink.eventsOf(SessionEventType::QueueUpdate);
    ASSERT_EQ(updates.size(), 2U);
    EXPECT_EQ(updates[1].steering, (std::vector<std::string>{"stop"}));
    EXPECT_EQ(updates[1].followUp, (std::vector<std::string>{"then this"}));
    EXPECT_EQ(m_tracker.count(), 2U);
}

TEST_F(PendingInputTrackerTest, DeliveryRemovesSteeringBeforeFollowUp) {
    m_tracker.queueFollowUp("same");
    m_tracker.queueSteering("same");
    m_tracker.delivered("same");
    EXPECT_TRUE(m_tracker.snapshot().steering.empty());
    EXPECT_EQ(m_tracker.snapshot().followUp.size(), 1U);
    m_tracker.delivered("same");
    EXPECT_EQ(m_tracker.count(), 0U);
}

TEST_F(PendingInputTrackerTest, DeliveryOfUnknownTextIsSilent) {
    m_tracker.queueSteering("a");
    const std::size_t before = m_sink.events().size();
    m_tracker.delivered("b");
    m_tracker.delivered("");
    EXPECT_EQ(m_sink.events().size(), before);
}

TEST_F(PendingInputTrackerTest, ClearReturnsWhatWasWaiting) {
    m_tracker.queueSteering("a");
    m_tracker.queueFollowUp("b");
    const QueuedInput cleared = m_tracker.clear();
    EXPECT_EQ(cleared.steering, (std::vector<std::string>{"a"}));
    EXPECT_EQ(cleared.followUp, (std::vector<std::string>{"b"}));
    EXPECT_EQ(m_tracker.count(), 0U);
    const auto updates = m_sink.eventsOf(SessionEventType::QueueUpdate);
    EXPECT_TRUE(updates.back().steering.empty());
}
