#include <gtest/gtest.h>

import std;
import pi.child_process_launcher;

TEST(ChildProcessLauncherTest, LaunchesStartedProcesses) {
    ChildProcessLauncher launcher;
    ProcessRequest request;
    request.command = "/bin/sh";
    request.args = {"-c", "echo hi"};
    auto child = launcher.launch(request);
    ASSERT_TRUE(child.has_value());
    EXPECT_EQ((*child)->readOutput(), "hi\n");
    EXPECT_EQ((*child)->waitForExit(std::chrono::seconds(5)), 0);
}

TEST(ChildProcessLauncherTest, ReportsLaunchFailures) {
    ChildProcessLauncher launcher;
    ProcessRequest request;
    request.command = "/definitely/not/here";
    const auto child = launcher.launch(request);
    ASSERT_FALSE(child.has_value());
    EXPECT_EQ(child.error().code, "spawn_failed");
}
