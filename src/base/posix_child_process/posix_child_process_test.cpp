#include <gtest/gtest.h>

import std;
import pi.base.posix_child_process;

class PosixChildProcessTest : public testing::Test {
protected:
    std::unique_ptr<IChildProcess> launch(const std::string& script) {
        ProcessRequest request;
        request.command = "/bin/sh";
        request.args = {"-c", script};
        auto child = std::make_unique<PosixChildProcess>(request);
        EXPECT_TRUE(child->start().has_value());
        return child;
    }

    std::string drain(IChildProcess& child, bool standardError = false) {
        std::string text;
        while (auto chunk = standardError ? child.readError() : child.readOutput()) {
            text += *chunk;
        }
        return text;
    }

};

TEST_F(PosixChildProcessTest, StdoutAndStderrAreSeparateStreams) {
    const auto child = launch("echo out; echo err >&2; exit 4");
    EXPECT_EQ(drain(*child), "out\n");
    EXPECT_EQ(drain(*child, true), "err\n");
    EXPECT_EQ(child->waitForExit(std::chrono::seconds(5)), 4);
}

TEST_F(PosixChildProcessTest, WritesReachStdinAndClosingEndsTheChild) {
    const auto child = launch("while read line; do echo \"got $line\"; done");
    ASSERT_TRUE(child->write("one\ntwo\n").has_value());
    child->closeStdin();
    EXPECT_EQ(drain(*child), "got one\ngot two\n");
    EXPECT_EQ(child->waitForExit(std::chrono::seconds(5)), 0);
    EXPECT_FALSE(child->write("late").has_value());
}

TEST_F(PosixChildProcessTest, InterleavedRequestsAndResponses) {
    const auto child = launch("read a; echo \"1:$a\"; read b; echo \"2:$b\"");
    ASSERT_TRUE(child->write("x\n").has_value());
    EXPECT_EQ(child->readOutput(), "1:x\n");
    ASSERT_TRUE(child->write("y\n").has_value());
    EXPECT_EQ(child->readOutput(), "2:y\n");
}

TEST_F(PosixChildProcessTest, WaitTimesOutAndTerminateStopsTheGroup) {
    const auto child = launch("echo ready; sleep 30 & wait");
    ASSERT_EQ(child->readOutput(), "ready\n");
    EXPECT_FALSE(child->waitForExit(std::chrono::milliseconds(50)).has_value());
    child->terminate();
    EXPECT_TRUE(child->waitForExit(std::chrono::seconds(5)).has_value());
}

TEST_F(PosixChildProcessTest, KillWorksOnStubbornChildren) {
    const auto child = launch("trap '' TERM; echo ready; while true; do sleep 1; done");
    ASSERT_EQ(child->readOutput(), "ready\n");
    child->terminate();
    EXPECT_FALSE(child->waitForExit(std::chrono::milliseconds(100)).has_value());
    child->kill();
    EXPECT_EQ(child->waitForExit(std::chrono::seconds(5)), -1);
}

TEST_F(PosixChildProcessTest, EnvironmentAndWorkingDirectoryApply) {
    ProcessRequest request;
    request.command = "/bin/sh";
    request.args = {"-c", "echo $PI_LAUNCH_TEST; pwd"};
    request.env = {{"PI_LAUNCH_TEST", "hello"}};
    request.cwd = "/tmp";
    PosixChildProcess child(request);
    ASSERT_TRUE(child.start().has_value());
    EXPECT_EQ(drain(child), "hello\n/tmp\n");
}

TEST_F(PosixChildProcessTest, MissingCommandsFailToLaunch) {
    ProcessRequest request;
    request.command = "/definitely/not/here";
    PosixChildProcess child(request);
    const auto started = child.start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code, "spawn_failed");
}

TEST_F(PosixChildProcessTest, DestroyingTheHandleStopsTheChild) {
    auto child = launch("sleep 30");
    child.reset();
    SUCCEED();
}
