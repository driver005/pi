#include <gtest/gtest.h>

import std;
import pi.support.iso_timestamp;

TEST(IsoTimestampTest, FormatsLikeJavaScript) {
    IsoTimestamp iso;
    EXPECT_EQ(iso.format(0), "1970-01-01T00:00:00.000Z");
    EXPECT_EQ(iso.format(1700000000123), "2023-11-14T22:13:20.123Z");
    EXPECT_EQ(iso.format(951782400999), "2000-02-29T00:00:00.999Z");
}

TEST(IsoTimestampTest, ParsesBack) {
    IsoTimestamp iso;
    EXPECT_EQ(iso.parse("2023-11-14T22:13:20.123Z"), 1700000000123LL);
    EXPECT_EQ(iso.parse("2023-11-14T22:13:20Z"), 1700000000000LL);
    EXPECT_EQ(iso.parse("2023-11-14T23:13:20.123+01:00"), 1700000000123LL);
    EXPECT_EQ(iso.parse("2023-11-14T22:13:20.5Z"), 1700000000500LL);
    EXPECT_EQ(iso.parse(iso.format(123456789012)), 123456789012LL);
}

TEST(IsoTimestampTest, RejectsGarbage) {
    IsoTimestamp iso;
    EXPECT_FALSE(iso.parse("").has_value());
    EXPECT_FALSE(iso.parse("yesterday").has_value());
    EXPECT_FALSE(iso.parse("2023-13-14T22:13:20Z").has_value());
    EXPECT_FALSE(iso.parse("2023-11-14T22:13:20Zjunk").has_value());
}
