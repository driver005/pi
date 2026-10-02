#include <gtest/gtest.h>

import std;
import pi.support.layered_ignore;

TEST(LayeredIgnoreTest, RootRulesApplyToNestedPaths) {
    LayeredIgnore ignore;
    ignore.addLayer("", "node_modules/\n*.tmp\n");
    EXPECT_TRUE(ignore.ignores("a/node_modules", true));
    EXPECT_TRUE(ignore.ignores("a/node_modules/x/SKILL.md", false));
    EXPECT_TRUE(ignore.ignores("x.tmp", false));
    EXPECT_FALSE(ignore.ignores("a/b.md", false));
}

TEST(LayeredIgnoreTest, DeeperLayersAreRelativeToTheirDirectory) {
    LayeredIgnore ignore;
    ignore.addLayer("sub", "secret.md\n");
    EXPECT_TRUE(ignore.ignores("sub/secret.md", false));
    EXPECT_FALSE(ignore.ignores("secret.md", false));
    EXPECT_FALSE(ignore.ignores("other/secret.md", false));
}

TEST(LayeredIgnoreTest, DeeperNegationReincludes) {
    LayeredIgnore ignore;
    ignore.addLayer("", "*.md\n");
    ignore.addLayer("docs", "!keep.md\n");
    EXPECT_TRUE(ignore.ignores("docs/other.md", false));
    EXPECT_FALSE(ignore.ignores("docs/keep.md", false));
}

TEST(LayeredIgnoreTest, EmptyByDefault) {
    LayeredIgnore ignore;
    EXPECT_TRUE(ignore.empty());
    EXPECT_FALSE(ignore.ignores("anything", false));
}
