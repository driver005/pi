#include <gtest/gtest.h>

import std;
import pi.base.boring_crypto;
import pi.base.posix_file_system;
import pi.base.posix_process_runner;
import pi.base.system_environment;
import pi.support.bash_command_executor;

class BashCommandExecutorTest : public testing::Test {
protected:
    Result<BashResult> run(const std::string& command, std::function<void(const std::string&)> onChunk = nullptr,
                           std::shared_ptr<AbortSignal> signal = nullptr) {
        return m_executor.execute(command, "/tmp", "", onChunk, signal);
    }

    PosixFileSystem m_files;
    PosixProcessRunner m_runner;
    BoringCrypto m_crypto;
    SystemEnvironment m_environment;
    BashCommandExecutor m_executor{m_runner, m_files, m_crypto, m_environment};
};

TEST_F(BashCommandExecutorTest, CapturesOutputAndExitCode) {
    std::string streamed;
    const auto result = run("printf 'hello\\n'; echo oops >&2; exit 3", [&](const std::string& text) { streamed += text; });
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->output.find("hello"), std::string::npos);
    EXPECT_NE(result->output.find("oops"), std::string::npos);
    EXPECT_EQ(result->exitCode, 3);
    EXPECT_FALSE(result->cancelled);
    EXPECT_FALSE(result->truncated);
    EXPECT_FALSE(result->fullOutputPath.has_value());
    EXPECT_EQ(streamed, result->output);
}

TEST_F(BashCommandExecutorTest, StripsColorFromOutput) {
    const auto result = run("printf '\\033[31mred\\033[0m\\n'");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->output, "red\n");
}

TEST_F(BashCommandExecutorTest, LongOutputIsTruncatedToTailAndSpilledToFile) {
    const auto result = run("seq 1 5000");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->truncated);
    ASSERT_TRUE(result->fullOutputPath.has_value());
    const auto full = m_files.readFile(*result->fullOutputPath);
    ASSERT_TRUE(full.has_value());
    EXPECT_TRUE(full->starts_with("1\n2\n3\n"));
    EXPECT_NE(result->output.find("5000"), std::string::npos);
    EXPECT_EQ(result->output.find("\n2\n"), std::string::npos);
    m_files.removeFile(*result->fullOutputPath);
}

TEST_F(BashCommandExecutorTest, AbortCancelsAndKeepsPartialOutput) {
    const auto signal = std::make_shared<AbortSignal>();
    std::thread aborter([signal] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        signal->abort();
    });
    const auto result = run("echo started; sleep 10", nullptr, signal);
    aborter.join();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->cancelled);
    EXPECT_FALSE(result->exitCode.has_value());
    EXPECT_NE(result->output.find("started"), std::string::npos);
}
