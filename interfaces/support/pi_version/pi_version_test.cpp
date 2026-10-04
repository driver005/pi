#include <gtest/gtest.h>

import std;
import pi.support.pi_version;

TEST(PiVersionTest, IsASemanticVersion) {
    const std::string version = PiVersion().value();
    EXPECT_EQ(std::ranges::count(version, '.'), 2);
}
