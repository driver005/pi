#include "src/base/system_clock/system_clock.h"

#include <gtest/gtest.h>

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
