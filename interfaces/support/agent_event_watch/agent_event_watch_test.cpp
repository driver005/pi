#include <gtest/gtest.h>

import std;
import pi.support.agent_event_watch;
import pi.support.tool_bridge;
import pi.testing.durable_harness_fixture;
import pi.testing.scripted_tool;

class AgentEventWatchTest : public ::testing::Test {
protected:
    AgentEventWatchTest() {
        EXPECT_TRUE(m_fixture.open().has_value());
    }

    std::vector<Json> collected() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_events;
    }

    bool waitFor(const std::function<bool(const std::vector<Json>&)>& done) {
        for (int attempt = 0; attempt < 2000; ++attempt) {
            if (done(collected())) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    std::shared_ptr<AgentEventWatch> watch() {
        auto attached = m_fixture.harness().watchEvents(m_fixture.root()->id());
        EXPECT_TRUE(attached.has_value()) << (attached ? "" : attached.error().message);
        (*attached)->start([this](const std::vector<Json>& events) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_events.insert(m_events.end(), events.begin(), events.end());
        });
        return *attached;
    }

    /** Index of the first event of `type` at or after `from`; -1 when none. */
    static int indexOf(const std::vector<Json>& events, const std::string& type, int from = 0) {
        for (int index = from; index < static_cast<int>(events.size()); ++index) {
            if (events[static_cast<std::size_t>(index)].at("type") == type) {
                return index;
            }
        }
        return -1;
    }

    static std::string types(const std::vector<Json>& events) {
        std::string out;
        for (const Json& event : events) {
            out += (out.empty() ? "" : " ") + event.value("type", std::string());
        }
        return out;
    }

    DurableHarnessFixture m_fixture;
    std::mutex m_mutex;
    std::vector<Json> m_events;
};

TEST_F(AgentEventWatchTest, ARunProducesItsEventsInTheOrderOfTheSpec) {
    auto stream = watch();
    EXPECT_EQ(stream->snapshot().at("type"), "snapshot");
    EXPECT_TRUE(stream->snapshot().at("entries").empty());
    m_fixture.faux().enqueue(m_fixture.faux().textResponse("hello there"));
    auto submission = m_fixture.root()->submit(m_fixture.input("hi"));
    ASSERT_TRUE(submission.has_value() && (*submission)->wait().has_value());
    ASSERT_TRUE(waitFor([](const std::vector<Json>& events) { return indexOf(events, "usage_changed") >= 0; }));
    const std::vector<Json> events = collected();
    const int runStart = indexOf(events, "run_start");
    const int turnStart = indexOf(events, "turn_start");
    const int answerStart = indexOf(events, "message_start", turnStart);
    const int answerEnd = indexOf(events, "message_end", answerStart);
    const int turnEnd = indexOf(events, "turn_end");
    const int runEnd = indexOf(events, "run_end");
    const int settled = indexOf(events, "submission", runEnd);
    const int usage = indexOf(events, "usage_changed");
    // The input is a message of its own; the run and turn begin in the commit that placed it.
    EXPECT_EQ(events[0].at("type"), "message_start");
    EXPECT_EQ(events[0].at("message").at("role"), "user");
    ASSERT_GE(runStart, 0) << types(events);
    EXPECT_LT(runStart, turnStart);
    EXPECT_LT(turnStart, answerStart);
    EXPECT_LT(answerStart, answerEnd);
    EXPECT_LT(answerEnd, turnEnd);
    EXPECT_LT(turnEnd, runEnd);
    EXPECT_LT(runEnd, settled);
    EXPECT_LT(settled, usage);
    EXPECT_EQ(events[static_cast<std::size_t>(answerEnd)].at("entry").at("kind"), "pi.assistant");
    EXPECT_EQ(events[static_cast<std::size_t>(settled)].at("record").at("status"), "done");
    EXPECT_EQ(stream->stop(), "stopped");
}

TEST_F(AgentEventWatchTest, AWatchAttachedAfterARunStartsFromTheirResult) {
    m_fixture.faux().enqueue(m_fixture.faux().textResponse("done"));
    auto submission = m_fixture.root()->submit(m_fixture.input("hi"));
    ASSERT_TRUE(submission.has_value() && (*submission)->wait().has_value());
    ASSERT_TRUE(m_fixture.root()->waitForIdle().has_value());
    // No view is held yet, so attaching builds it from the committed state.
    auto stream = watch();
    EXPECT_EQ(stream->snapshot().at("entries").size(), 2u);
    EXPECT_FALSE(stream->snapshot().contains("run"));
    EXPECT_TRUE(stream->snapshot().at("usage").at("models").contains("faux/m"));
}

TEST_F(AgentEventWatchTest, ToolCallsStartAndEndAroundTheirResultEntry) {
    auto tools = std::make_shared<ToolSetCache>([](const std::string&) { return ToolSetCache::ToolSet{std::make_shared<ScriptedTool>("echo", "pong")}; });
    ASSERT_TRUE(m_fixture.registry().install(ToolBridge(tools, "/default").extension("coding-tools")).has_value());
    auto stream = watch();
    m_fixture.faux().enqueue(m_fixture.faux().toolCallResponse("echo", Json::object({{"arg", "x"}}), "call-1"));
    m_fixture.faux().enqueue(m_fixture.faux().textResponse("done"));
    auto submission = m_fixture.root()->submit(m_fixture.input("go"));
    ASSERT_TRUE(submission.has_value() && (*submission)->wait().has_value());
    ASSERT_TRUE(waitFor([](const std::vector<Json>& events) { return indexOf(events, "run_end") >= 0; }));
    const std::vector<Json> events = collected();
    const int start = indexOf(events, "tool_execution_start");
    const int end = indexOf(events, "tool_execution_end");
    ASSERT_GE(start, 0) << types(events);
    ASSERT_GE(end, 0) << types(events);
    EXPECT_EQ(events[static_cast<std::size_t>(start)].at("toolCallId"), "call-1");
    EXPECT_EQ(events[static_cast<std::size_t>(start)].at("toolName"), "echo");
    EXPECT_EQ(events[static_cast<std::size_t>(start)].at("args"), (Json{{"arg", "x"}}));
    EXPECT_LT(start, end);
    // The end directly precedes the message of its result.
    EXPECT_EQ(events[static_cast<std::size_t>(end)].at("entry").at("model")[0].at("role"), "toolResult");
    EXPECT_EQ(events[static_cast<std::size_t>(end) + 1].at("type"), "message_start");
    EXPECT_EQ(events[static_cast<std::size_t>(end) + 1].at("message").at("role"), "toolResult");
    stream->stop();
}

TEST_F(AgentEventWatchTest, ClosingTheHarnessEndsTheWatch) {
    auto stream = watch();
    ASSERT_TRUE(m_fixture.harness().close().has_value());
    EXPECT_EQ(stream->wait(), "session_closed");
    EXPECT_EQ(stream->stop(), "session_closed");
}

TEST_F(AgentEventWatchTest, AWatchBehindByTooManyBatchesReceivesOneSnapshotOfTheNewestState) {
    auto attached = m_fixture.harness().watchEvents(m_fixture.root()->id());
    ASSERT_TRUE(attached.has_value());
    // Not started: every commit queues one batch (`entry_appended`) until the limit replaces them all by a snapshot. A commit
    // delivers its publication before it returns, so everything is queued when the loop ends.
    const std::size_t commits = AgentEventWatch::kMaxPendingBatches + 20;
    for (std::size_t index = 0; index < commits; ++index) {
        ASSERT_TRUE(m_fixture.root()->commit([&](Transaction& tx) -> Result<void> {
            auto entry = tx.appendEntry(m_fixture.root()->id(), Json::object({{"kind", "note"}}));
            return entry ? Result<void>() : std::unexpected(entry.error());
        }).has_value());
    }
    std::mutex mutex;
    std::vector<std::vector<Json>> batches;
    (*attached)->start([&](const std::vector<Json>& events) {
        const std::lock_guard<std::mutex> lock(mutex);
        batches.push_back(events);
    });
    for (int attempt = 0; attempt < 1000; ++attempt) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (batches.size() >= 20u) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(batches.size(), 20u);
    ASSERT_EQ(batches[0].size(), 1u);
    EXPECT_EQ(batches[0][0].at("type"), "snapshot");
    EXPECT_EQ(batches[0][0].at("entries").size(), AgentEventWatch::kMaxPendingBatches + 1);
    for (std::size_t index = 1; index < batches.size(); ++index) {
        ASSERT_EQ(batches[index].size(), 1u);
        EXPECT_EQ(batches[index][0].at("type"), "entry_appended");
    }
    (*attached)->stop();
}
