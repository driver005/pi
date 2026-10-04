#include <gtest/gtest.h>

import std;
import pi.base.system_clock;
import pi.platform.i_clock;

TEST(SystemClockTest, ReturnsPlausibleEpochMillis) {
    SystemClock clock;
    const IClock& asInterface = clock;
    EXPECT_GT(asInterface.nowMs(), 1'700'000'000'000);
}

TEST(SystemClockTest, IsMonotonicEnoughForTwoReads) {
    SystemClock clock;
    const auto first = clock.nowMs();
    const auto second = clock.nowMs();
    EXPECT_GE(second, first - 1);
}
