#include <gtest/gtest.h>

import std;
import pi.support.short_hash;

// Expected values were produced by the TypeScript implementation (utils/hash.ts).
TEST(ShortHashTest, MatchesTypeScriptOutput) {
    ShortHash hash;
    EXPECT_EQ(hash.of(""), "k4n83c7h0j2b");
    EXPECT_EQ(hash.of("abc"), "y0biex7f9bbh");
    EXPECT_EQ(hash.of("call_1|fc_abc+/="), "1cks14r1vf0r6g");
    EXPECT_EQ(hash.of("h\xC3\xA9llo w\xC3\xB6rld"), "1slrdvn1t61j5h");
    EXPECT_EQ(hash.of("\xF0\x9F\x98\x80 emoji"), "10l7wx41397zoo");
    EXPECT_EQ(hash.of(std::string(500, 'x')), "130aesn1aldl4j");
}
