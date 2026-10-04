#include <gtest/gtest.h>

import std;
import pi.support.crc32;

TEST(Crc32Test, MatchesTheKnownCheckValues) {
    Crc32 crc;
    EXPECT_EQ(crc.compute(""), 0U);
    EXPECT_EQ(crc.compute("123456789"), 0xCBF43926U);
    EXPECT_EQ(crc.compute("The quick brown fox jumps over the lazy dog"), 0x414FA339U);
}
