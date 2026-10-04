#include <gtest/gtest.h>

import std;
import pi.support.timings;
import pi.testing.fixed_clock;

TEST(TimingsTest, MarksMeasureTheTimeSinceThePreviousOne) {
    FixedClock clock;
    Timings timings(clock, true);
    timings.reset();
    clock.advance(120);
    timings.time("settings");
    clock.advance(30);
    timings.time("models");
    timings.time("ext", "extensions");
    const std::string report = timings.report();
    EXPECT_NE(report.find("--- Startup Timings: main ---"), std::string::npos);
    EXPECT_NE(report.find("  settings: 120ms"), std::string::npos);
    EXPECT_NE(report.find("  models: 30ms"), std::string::npos);
    EXPECT_NE(report.find("  TOTAL: 150ms"), std::string::npos);
    EXPECT_NE(report.find("Startup Timings: extensions"), std::string::npos);
}

TEST(TimingsTest, DisabledTimingsRecordNothing) {
    FixedClock clock;
    Timings timings(clock, false);
    timings.time("x");
    EXPECT_TRUE(timings.report().empty());
    EXPECT_FALSE(timings.enabled());
}
