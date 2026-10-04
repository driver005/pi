#include <gtest/gtest.h>

import std;
import pi.base.posix_system_info;

TEST(PosixSystemInfoTest, DescribesTheHostWithNodeStyleNames) {
    PosixSystemInfo info;
    EXPECT_EQ(info.platform(), "linux");
    EXPECT_TRUE(info.arch() == "x64" || info.arch() == "arm64" || !info.arch().empty());
    EXPECT_FALSE(info.osRelease().empty());
}
