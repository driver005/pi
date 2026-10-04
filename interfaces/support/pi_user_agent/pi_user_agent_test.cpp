#include <gtest/gtest.h>

import std;
import pi.support.pi_user_agent;
import pi.testing.fake_system_info;

TEST(PiUserAgentTest, NamesPlatformReleaseAndArchitecture) {
    FakeSystemInfo system;
    EXPECT_EQ(PiUserAgent().value(system), "pi (linux 6.1.0-test; x64)");
}
