#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.base.boring_crypto;
import pi.base.posix_file_system;
import pi.base.posix_process_runner;
import pi.base.system_environment;
import pi.tools.bash_tool;

class BashToolTest : public testing::Test {
protected:
    Result<AgentToolResult> run(const Json& params, const std::shared_ptr<AbortSignal>& signal = nullptr,
                                const ToolUpdateCallback& update = nullptr) {
        return m_tool.execute("c1", params, signal, update);
    }

    std::string textOf(const AgentToolResult& result) {
        return std::get<TextContent>(result.content[0]).text;
    }

    PosixProcessRunner m_runner;
    PosixFileSystem m_fs;
    BoringCrypto m_crypto;
    SystemEnvironment m_env;
    std::string m_dir = [] {
        const std::string dir = std::string(std::getenv("TEST_TMPDIR")) + "/bash_" +
                                testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }();
    BashTool m_tool{m_runner, m_fs, m_crypto, m_env, m_dir};
};

TEST_F(BashToolTest, ReturnsOutputAndStructuredContent) {
    const auto result = run({{"command", "echo hello; echo oops 1>&2"}});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(textOf(*result), "hello\noops\n");
    EXPECT_FALSE(result->isError);
    EXPECT_EQ(result->structuredContent["exit_code"], 0);
    EXPECT_EQ(result->structuredContent["output"], "hello\noops\n");
    EXPECT_FALSE(result->structuredContent["truncated"].get<bool>());
}

TEST_F(BashToolTest, NoOutputPlaceholder) {
    const auto result = run({{"command", "true"}});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(textOf(*result), "(no output)");
}

TEST_F(BashToolTest, NonZeroExitIsErrorResultWithStatus) {
    const auto result = run({{"command", "echo before; exit 3"}});
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->isError);
    EXPECT_EQ(textOf(*result), "before\n\n\nCommand exited with code 3");
    EXPECT_EQ(result->structuredContent["exit_code"], 3);
}

TEST_F(BashToolTest, RunsInConfiguredWorkingDirectory) {
    const auto result = run({{"command", "pwd"}});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(textOf(*result), m_fs.realPath(m_dir) + "\n");
}

TEST_F(BashToolTest, TimeoutProducesErrorWithPartialOutput) {
    const auto result = run({{"command", "echo started; sleep 30"}, {"timeout", 0.3}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "started\n\n\nCommand timed out after 0.3 seconds");
}

TEST_F(BashToolTest, AbortKillsCommand) {
    auto signal = std::make_shared<AbortSignal>();
    std::thread aborter([signal] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        signal->abort();
    });
    const auto result = run({{"command", "sleep 30"}}, signal);
    aborter.join();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Command aborted");
}

TEST_F(BashToolTest, InvalidTimeoutAndMissingCwd) {
    EXPECT_EQ(run({{"command", "true"}, {"timeout", -1}}).error().message,
              "Invalid timeout: must be a finite number of seconds");
    EXPECT_EQ(run({{"command", "true"}, {"timeout", 3e9}}).error().message,
              "Invalid timeout: maximum is 2147483.647 seconds");
    BashTool missing(m_runner, m_fs, m_crypto, m_env, m_dir + "/nope");
    EXPECT_EQ(missing.execute("c", {{"command", "true"}}, nullptr, nullptr).error().message,
              "Working directory does not exist: " + m_dir + "/nope\nCannot execute bash commands.");
}

TEST_F(BashToolTest, LongOutputIsTailTruncatedAndSavedToFile) {
    const auto result = run({{"command", "seq 1 3000"}});
    ASSERT_TRUE(result.has_value());
    const std::string text = textOf(*result);
    EXPECT_NE(text.find("[Showing lines 1001-3000 of 3000. Full output: "), std::string::npos);
    ASSERT_TRUE(result->details.contains("fullOutputPath"));
    const std::string saved = m_fs.readFile(result->details["fullOutputPath"].get<std::string>()).value();
    EXPECT_EQ(saved.substr(0, 6), "1\n2\n3\n");
    EXPECT_EQ(text.substr(0, 5), "1001\n");
}

TEST_F(BashToolTest, StreamsUpdatesForSlowCommands) {
    int updates = 0;
    const auto result = run({{"command", "echo one; sleep 0.3; echo two"}}, nullptr,
                            [&](const AgentToolResult&) { ++updates; });
    ASSERT_TRUE(result.has_value());
    EXPECT_GE(updates, 2);
}

TEST_F(BashToolTest, CommandPrefixIsPrepended) {
    BashTool prefixed(m_runner, m_fs, m_crypto, m_env, m_dir, "", "export GREETING=hi");
    const auto result = prefixed.execute("c", {{"command", "echo $GREETING"}}, nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(textOf(*result), "hi\n");
}
