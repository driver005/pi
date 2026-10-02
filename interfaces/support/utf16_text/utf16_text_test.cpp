#include <gtest/gtest.h>

import std;
import pi.support.utf16_text;

TEST(Utf16TextTest, CountsCodeUnits) {
    const Utf16Text text;
    EXPECT_EQ(text.length(""), 0U);
    EXPECT_EQ(text.length("abc"), 3U);
    EXPECT_EQ(text.length("\xC3\xBC"), 1U);
    EXPECT_EQ(text.length("\xE6\xB0\xB4"), 1U);
    EXPECT_EQ(text.length("\xF0\x90\x85\x91"), 2U);
    EXPECT_EQ(text.length("a\xF0\x9F\x98\x80z"), 4U);
}

TEST(Utf16TextTest, MapsUnitsToByteOffsets) {
    const Utf16Text text;
    const std::string mixed = "a\xC3\xBC\xF0\x9F\x98\x80z";
    EXPECT_EQ(text.byteOffset(mixed, 0), 0U);
    EXPECT_EQ(text.byteOffset(mixed, 1), 1U);
    EXPECT_EQ(text.byteOffset(mixed, 2), 3U);
    EXPECT_FALSE(text.byteOffset(mixed, 3).has_value());
    EXPECT_EQ(text.byteOffset(mixed, 4), 7U);
    EXPECT_EQ(text.byteOffset(mixed, 5), 8U);
    EXPECT_FALSE(text.byteOffset(mixed, 6).has_value());
}

TEST(Utf16TextTest, FindsCodePointBoundaries) {
    const Utf16Text text;
    const std::string value = "a\xC3\xBCz";
    EXPECT_TRUE(text.boundary(value, 0));
    EXPECT_TRUE(text.boundary(value, 1));
    EXPECT_FALSE(text.boundary(value, 2));
    EXPECT_TRUE(text.boundary(value, 3));
    EXPECT_TRUE(text.boundary(value, 4));
}
