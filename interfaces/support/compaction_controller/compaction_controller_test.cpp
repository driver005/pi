#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.compaction_controller;
import pi.testing.fake_settings_manager;
import pi.testing.recording_session_sink;
import pi.testing.session_harness;

class CompactionControllerTest : public testing::Test {
protected:
    CompactionControllerTest()
        : m_agent(m_harness.makeAgent()),
          m_settings(Json{{"compaction", Json{{"keepRecentTokens", 500}, {"reserveTokens", 1000}}}}),
          m_refresher(*m_agent, m_harness.session()),
          m_generator(m_harness.clock(), m_harness.ids(), m_harness.sleeper()),
          m_compactor(m_generator),
          m_controller(*m_agent, m_harness.session(), m_settings, m_refresher, m_sink, m_compactor,
                       [this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
                           return m_harness.provider().stream(model, context, options);
                       }) {}

    void seedTurns(int turns) {
        for (int i = 0; i < turns; ++i) {
            UserMessage user;
            user.content = std::string(1200, 'u');
            user.timestamp = ++m_stamp;
            ASSERT_TRUE(m_harness.session().appendMessage(user).has_value());
            AssistantMessage reply;
            reply.content.push_back(TextContent{std::string(1200, 'a'), std::nullopt});
            reply.stopReason = StopReason::Stop;
            reply.timestamp = ++m_stamp;
            ASSERT_TRUE(m_harness.session().appendMessage(reply).has_value());
        }
        m_refresher.refresh();
    }

    SessionHarness m_harness;
    std::unique_ptr<IAgent> m_agent;
    FakeSettingsManager m_settings;
    SessionContextRefresher m_refresher;
    RecordingSessionSink m_sink;
    SummaryGenerator m_generator;
    Compactor m_compactor;
    CompactionController m_controller;
    std::int64_t m_stamp = 1000;
};

TEST_F(CompactionControllerTest, ManualCompactionAppendsEntryAndReloadsContext) {
    seedTurns(4);
    m_harness.provider().enqueue(m_harness.provider().textResponse("## Goal\nsummary"));
    const auto result = m_controller.compactManual(std::string("focus on auth"));
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->summary.find("summary"), std::string::npos);
    EXPECT_GT(result->tokensBefore, 0);
    ASSERT_TRUE(result->estimatedTokensAfter.has_value());
    EXPECT_LT(*result->estimatedTokensAfter, result->tokensBefore);

    const auto path = m_harness.session().branchPath();
    EXPECT_EQ(path.back().type, "compaction");
    const auto messages = m_agent->messages();
    EXPECT_TRUE(std::holds_alternative<CustomMessage>(messages.front()));
    EXPECT_EQ(std::get<CustomMessage>(messages.front()).role, "compactionSummary");

    const auto starts = m_sink.eventsOf(SessionEventType::CompactionStart);
    const auto ends = m_sink.eventsOf(SessionEventType::CompactionEnd);
    ASSERT_EQ(starts.size(), 1U);
    ASSERT_EQ(ends.size(), 1U);
    EXPECT_EQ(starts[0].reason, "manual");
    EXPECT_TRUE(ends[0].result.has_value());
    EXPECT_FALSE(ends[0].aborted);
    EXPECT_FALSE(m_controller.compacting());
}

TEST_F(CompactionControllerTest, ManualCompactionReportsWhyNothingHappened) {
    const auto empty = m_controller.compactManual(std::nullopt);
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().message, "Nothing to compact (session too small)");
    const auto ends = m_sink.eventsOf(SessionEventType::CompactionEnd);
    ASSERT_EQ(ends.size(), 1U);
    EXPECT_EQ(ends[0].errorMessage, "Compaction failed: Nothing to compact (session too small)");

    seedTurns(4);
    m_harness.provider().enqueue(m_harness.provider().textResponse("summary"));
    const auto first = m_controller.compactManual(std::nullopt);
    ASSERT_TRUE(first.has_value()) << first.error().message;
    const auto again = m_controller.compactManual(std::nullopt);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().message, "Already compacted");
}

TEST_F(CompactionControllerTest, SummaryFailureIsReportedAndKeepsSessionUntouched) {
    seedTurns(4);
    const std::size_t before = m_harness.session().entryCount();
    AssistantMessage failed = m_harness.provider().textResponse("x");
    failed.stopReason = StopReason::Error;
    failed.errorMessage = "400 bad request";
    m_harness.provider().enqueue(failed);
    const auto result = m_controller.compactManual(std::nullopt);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(m_harness.session().entryCount(), before);
    const auto ends = m_sink.eventsOf(SessionEventType::CompactionEnd);
    ASSERT_EQ(ends.size(), 1U);
    EXPECT_EQ(ends[0].errorMessage, "Compaction failed: Summarization failed: 400 bad request");
    EXPECT_FALSE(ends[0].aborted);
}

TEST_F(CompactionControllerTest, AbortDuringSummaryReportsCancelled) {
    seedTurns(4);
    m_harness.provider().enqueue([this](const TranscriptContext&, const StreamOptions&, const Model&) {
        m_controller.abort();
        return m_harness.provider().textResponse("late");
    });
    const auto result = m_controller.compactManual(std::nullopt);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Compaction cancelled");
    const auto ends = m_sink.eventsOf(SessionEventType::CompactionEnd);
    ASSERT_EQ(ends.size(), 1U);
    EXPECT_TRUE(ends[0].aborted);
    EXPECT_FALSE(ends[0].errorMessage.has_value());
}

TEST_F(CompactionControllerTest, AutoCompactionContinuesOnlyForRetryOrQueuedMessages) {
    seedTurns(4);
    m_harness.provider().enqueue(m_harness.provider().textResponse("summary one"));
    EXPECT_FALSE(m_controller.runAuto("threshold", false));
    ASSERT_FALSE(m_sink.eventsOf(SessionEventType::CompactionEnd).empty());
    EXPECT_TRUE(m_sink.eventsOf(SessionEventType::CompactionEnd).back().result.has_value())
        << m_sink.eventsOf(SessionEventType::CompactionEnd).back().errorMessage.value_or("");

    seedTurns(4);
    m_harness.provider().enqueue(m_harness.provider().textResponse("summary two"));
    EXPECT_TRUE(m_controller.runAuto("overflow", true));
    EXPECT_TRUE(m_sink.eventsOf(SessionEventType::CompactionEnd).back().willRetry);

    seedTurns(4);
    m_harness.provider().enqueue(m_harness.provider().textResponse("summary three"));
    UserMessage queued;
    queued.content = std::string("later");
    m_agent->followUp(queued);
    EXPECT_TRUE(m_controller.runAuto("threshold", false));
}

TEST_F(CompactionControllerTest, AutoCompactionWithNothingToCompactIsSilent) {
    EXPECT_FALSE(m_controller.runAuto("threshold", false));
    EXPECT_TRUE(m_sink.events().empty());
}

TEST_F(CompactionControllerTest, AutoFailureTextDependsOnReason) {
    seedTurns(4);
    AssistantMessage failed = m_harness.provider().textResponse("x");
    failed.stopReason = StopReason::Error;
    failed.errorMessage = "400 nope";
    m_harness.provider().enqueue(failed);
    EXPECT_FALSE(m_controller.runAuto("overflow", true));
    EXPECT_EQ(m_sink.eventsOf(SessionEventType::CompactionEnd).back().errorMessage,
              "Context overflow recovery failed: Summarization failed: 400 nope");
    m_harness.provider().enqueue(failed);
    EXPECT_FALSE(m_controller.runAuto("threshold", false));
    EXPECT_EQ(m_sink.eventsOf(SessionEventType::CompactionEnd).back().errorMessage,
              "Auto-compaction failed: Summarization failed: 400 nope");
}
