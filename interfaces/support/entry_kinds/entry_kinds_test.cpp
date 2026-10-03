#include <gtest/gtest.h>

import std;
import pi.support.entry_kinds;

TEST(EntryKindsTest, KindsAreTheProtocolStrings) {
    EntryKinds kinds;
    EXPECT_EQ(kinds.user(), "pi.user");
    EXPECT_EQ(kinds.assistant(), "pi.assistant");
    EXPECT_EQ(kinds.system(), "pi.system");
    EXPECT_EQ(kinds.toolResult(), "pi.tool-result");
    EXPECT_EQ(kinds.reset(), "pi.reset");
    EXPECT_EQ(kinds.compaction(), "pi.compaction");
}

TEST(EntryKindsTest, IsMatchesByKind) {
    EntryKinds kinds;
    EXPECT_TRUE(kinds.is(Json::object({{"kind", "pi.user"}}), kinds.user()));
    EXPECT_FALSE(kinds.is(Json::object({{"kind", "pi.user"}}), kinds.system()));
    EXPECT_FALSE(kinds.is(std::nullopt, kinds.user()));
}
