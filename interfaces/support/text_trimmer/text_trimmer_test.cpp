#include <gtest/gtest.h>

import std;
import pi.support.text_trimmer;

TEST(TextTrimmerTest, TrimsOnlyOuterWhitespace) {
    const TextTrimmer trimmer;
    EXPECT_EQ(trimmer.trim(" \t a b \r\n"), "a b");
    EXPECT_EQ(trimmer.trim("  \n"), "");
    EXPECT_EQ(trimmer.trim(""), "");
    EXPECT_EQ(trimmer.trim("x"), "x");
}

TEST(TextTrimmerTest, StripsOneLeadingByteOrderMark) {
    const TextTrimmer trimmer;
    EXPECT_EQ(trimmer.stripBom("\xEF\xBB\xBF{}"), "{}");
    EXPECT_EQ(trimmer.stripBom("{}"), "{}");
    EXPECT_EQ(trimmer.stripBom("\xEF\xBB"), "\xEF\xBB");
}
