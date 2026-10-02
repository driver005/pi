#include "src/base/posix_process_runner/posix_process_runner.h"

#include <gtest/gtest.h>

#include <thread>

class PosixProcessRunnerTest : public testing::Test {
protected:
    ProcessRequest shell(const std::string& script) {
        ProcessRequest request;
        request.command = "/bin/sh";
        request.args = {"-c", script};
        return request;
    }

    PosixProcessRunner m_runner;
};

TEST_F(PosixProcessRunnerTest, CapturesMergedOutputAndExitCode) {
    const auto result = m_runner.run(shell("echo out; echo err 1>&2; exit 3"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->exitCode, 3);
    EXPECT_EQ(result->output, "out\nerr\n");
    EXPECT_FALSE(result->timedOut);
}

TEST_F(PosixProcessRunnerTest, PassesStdinCwdAndEnv) {
    ProcessRequest request = shell("read line; echo \"$line $PI_X $(pwd)\"");
    request.stdinData = "hello\n";
    request.cwd = "/tmp";
    request.env["PI_X"] = "val";
    const auto result = m_runner.run(request);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->output, "hello val /tmp\n");
}

TEST_F(PosixProcessRunnerTest, LargeStdinAndOutput) {
    ProcessRequest request = shell("cat");
    request.stdinData = std::string(300000, 'x');
    const auto result = m_runner.run(request);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->output.size(), 300000U);
}

TEST_F(PosixProcessRunnerTest, TimeoutKillsProcessGroup) {
    ProcessRequest request = shell("sleep 30 & sleep 30");
    request.timeout = std::chrono::milliseconds(200);
    const auto started = std::chrono::steady_clock::now();
    const auto result = m_runner.run(request);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->timedOut);
    EXPECT_EQ(result->termSignal, SIGTERM);
    EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST_F(PosixProcessRunnerTest, AbortSignalStopsProcess) {
    ProcessRequest request = shell("sleep 30");
    request.signal = std::make_shared<AbortSignal>();
    std::thread aborter([signal = request.signal] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        signal->abort();
    });
    const auto result = m_runner.run(request);
    aborter.join();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->aborted);
}

TEST_F(PosixProcessRunnerTest, StreamsOutputToCallback) {
    ProcessRequest request = shell("echo a; echo b");
    std::string seen;
    request.onOutput = [&](std::string_view chunk) { seen.append(chunk); };
    request.captureOutput = false;
    const auto result = m_runner.run(request);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(seen, "a\nb\n");
    EXPECT_TRUE(result->output.empty());
}

TEST_F(PosixProcessRunnerTest, MissingCommandIsSpawnError) {
    ProcessRequest request;
    request.command = "definitely-not-a-command-xyz";
    const auto result = m_runner.run(request);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "spawn_failed");
}

TEST_F(PosixProcessRunnerTest, CleanEnvironmentWhenNotInherited) {
    ProcessRequest request = shell("echo \"[$HOME]\"");
    request.inheritEnv = false;
    const auto result = m_runner.run(request);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->output, "[]\n");
}
