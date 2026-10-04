#include <gtest/gtest.h>

import std;
import pi.support.directory_execution_env;

TEST(DirectoryExecutionEnvTest, ReportsItsDirectory) {
    const DirectoryExecutionEnv env("/work/project");
    EXPECT_EQ(env.cwd(), "/work/project");
}
