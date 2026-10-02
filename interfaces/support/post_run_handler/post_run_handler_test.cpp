#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.post_run_handler;
import pi.support.session_context_refresher;
import pi.testing.fake_settings_manager;
import pi.testing.recording_session_sink;
import pi.testing.session_harness;

class PostRunHandlerTest : public testing::Test {
protected:
    PostRunHandlerTest()
        : m_agent(m_harness.makeAgent()),
          m_refresher(*m_agent, m_harness.session()),
          m_omitter(m_harness.session(), m_sink, m_refresher),
          m_retry(m_settings, m_harness.sleeper(), m_omitter, m_sink),
          m_generator(m_harness.clock(), m_harness.ids(), m_harness.sleeper()),
          m_compactor(m_generator),
          m_compaction(*m_agent, m_harness.session(), m_settings, m_refresher, m_sink, m_compactor,
                       [this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
                           return m_harness.provider().stream(model, context, options);
                       }),
          m_handler(*m_agent, m_harness.session(), m_settings, m_retry, m_compaction, m_omitter, m_sink) {}

    AssistantMessage stored(StopReason reason, const std::string& error = "") {
        AssistantMessage message = m_harness.provider().textResponse("reply");
        message.stopReason = reason;
        if (!error.empty()) {
            message.errorMessage = error;
        }
        message.timestamp = ++m_stamp;
        m_harness.session().appendMessage(message);
        return message;
    }

    bool never() { return false; }

    SessionHarness m_harness;
    std::unique_ptr<IAgent> m_agent;
    FakeSettingsManager m_settings{Json{{"retry", Json{{"maxRetries", 1}, {"baseDelayMs", 5}}}}};
    SessionContextRefresher m_refresher;
    RecordingSessionSink m_sink;
    RecoveryAttemptOmitter m_omitter;
    AutoRetryController m_retry;
    SummaryGenerator m_generator;
    Compactor m_compactor;
    CompactionController m_compaction;
    PostRunHandler m_handler;
    std::int64_t m_stamp = 1000;
};

TEST_F(PostRunHandlerTest, AbortedRunNeverContinues) {
    EXPECT_FALSE(m_handler.afterRun(stored(StopReason::Error, "503 overloaded"), {}, [] { return true; }));
    EXPECT_TRUE(m_sink.eventsOf(SessionEventType::AutoRetryStart).empty());
}

TEST_F(PostRunHandlerTest, NoAssistantMessageContinuesOnlyForQueuedInput) {
    EXPECT_FALSE(m_handler.afterRun(std::nullopt, {}, [] { return false; }));
    UserMessage queued;
    queued.content = std::string("later");
    m_agent->followUp(queued);
    EXPECT_TRUE(m_handler.afterRun(std::nullopt, {}, [] { return false; }));
}

TEST_F(PostRunHandlerTest, TransientErrorRetriesUntilTheBudgetIsSpent) {
    EXPECT_TRUE(m_handler.afterRun(stored(StopReason::Error, "503 overloaded"), {}, [] { return false; }));
    EXPECT_EQ(m_retry.attempt(), 1);
    EXPECT_FALSE(m_handler.afterRun(stored(StopReason::Error, "503 overloaded"), {}, [] { return false; }));
    const auto ends = m_sink.eventsOf(SessionEventType::AutoRetryEnd);
    ASSERT_EQ(ends.size(), 1U);
    EXPECT_FALSE(ends[0].success);
}

TEST_F(PostRunHandlerTest, HealthyResponseNeedsNothing) {
    EXPECT_FALSE(m_handler.afterRun(stored(StopReason::Stop), {}, [] { return false; }));
    EXPECT_TRUE(m_sink.events().empty());
}

TEST_F(PostRunHandlerTest, ContextForNextResponseIsTheProjectionWhenSmall) {
    stored(StopReason::Stop);
    EXPECT_EQ(m_handler.contextForNextResponse().size(), 1U);
}
