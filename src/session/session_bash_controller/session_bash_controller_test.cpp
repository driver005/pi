#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.boring_crypto;
import pi.base.posix_file_system;
import pi.base.posix_process_runner;
import pi.base.system_environment;
import pi.session.session_bash_controller;
import pi.testing.fake_settings_manager;
import pi.testing.recording_session_sink;
import pi.testing.session_harness;

class SessionBashControllerTest : public testing::Test {
protected:
    SessionBashControllerTest()
        : m_agent(m_harness.makeAgent()),
          m_refresher(*m_agent, m_harness.session()),
          m_controller(*m_agent, m_harness.session(), m_settings, m_refresher, m_sink, m_executor,
                       m_harness.clock()) {}

    SessionHarness m_harness{"/tmp"};
    std::unique_ptr<IAgent> m_agent;
    FakeSettingsManager m_settings{Json{{"shellCommandPrefix", "FOO=bar"}}};
    SessionContextRefresher m_refresher;
    RecordingSessionSink m_sink;
    PosixFileSystem m_files;
    PosixProcessRunner m_runner;
    BoringCrypto m_crypto;
    SystemEnvironment m_environment;
    BashCommandExecutor m_executor{m_runner, m_files, m_crypto, m_environment};
    SessionBashController m_controller;
};

TEST_F(SessionBashControllerTest, ExecutesAppliesPrefixAndRecordsInSession) {
    std::string streamed;
    const auto result = m_controller.execute("echo \"value=$FOO\"", [&](const std::string& text) { streamed += text; },
                                             false, std::string("run-1"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->output, "value=bar\n");
    EXPECT_EQ(streamed, result->output);
    const auto updates = m_sink.eventsOf(SessionEventType::BashExecutionUpdate);
    ASSERT_FALSE(updates.empty());
    EXPECT_EQ(updates[0].id, "run-1");

    const auto messages = m_agent->messages();
    ASSERT_EQ(messages.size(), 1U);
    const auto& recorded = std::get<CustomMessage>(messages[0]);
    EXPECT_EQ(recorded.role, "bashExecution");
    EXPECT_EQ(recorded.data["command"], "echo \"value=$FOO\"");
    EXPECT_EQ(recorded.data["exitCode"], 0);
    EXPECT_FALSE(recorded.data.contains("excludeFromContext"));
}

TEST_F(SessionBashControllerTest, ExcludedOutputIsMarked) {
    m_settings.removeGlobal("shellCommandPrefix");
    ASSERT_TRUE(m_controller.execute("echo secret", nullptr, true, std::nullopt).has_value());
    const auto messages = m_agent->messages();
    EXPECT_EQ(std::get<CustomMessage>(messages[0]).data["excludeFromContext"], true);
}

TEST_F(SessionBashControllerTest, ResultsWaitWhileTheAgentRuns) {
    m_harness.provider().enqueue([this](const TranscriptContext&, const StreamOptions&, const Model&) {
        m_controller.record("ls", BashResult{"file\n", 0, false, false, std::nullopt}, false);
        EXPECT_TRUE(m_controller.hasPending());
        return m_harness.provider().textResponse("done");
    });
    ASSERT_TRUE(m_agent->promptText("go", {}).has_value());
    EXPECT_TRUE(m_controller.hasPending());
    const std::size_t before = m_harness.session().entryCount();
    m_controller.flushPending();
    EXPECT_FALSE(m_controller.hasPending());
    EXPECT_EQ(m_harness.session().entryCount(), before + 1);
    const auto messages = m_agent->messages();
    EXPECT_EQ(std::get<CustomMessage>(messages.back()).role, "bashExecution");
}

TEST_F(SessionBashControllerTest, AbortCancelsRunningCommand) {
    std::thread aborter([this] {
        while (!m_controller.running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        m_controller.abort();
    });
    const auto result = m_controller.execute("sleep 10", nullptr, false, std::nullopt);
    aborter.join();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->cancelled);
    EXPECT_FALSE(m_controller.running());
    const auto messages = m_agent->messages();
    EXPECT_EQ(std::get<CustomMessage>(messages.back()).data["cancelled"], true);
}
