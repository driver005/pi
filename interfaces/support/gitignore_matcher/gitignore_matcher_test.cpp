#include <gtest/gtest.h>

import std;
import pi.support.gitignore_matcher;

TEST(GitignoreMatcherTest, UnanchoredPatternMatchesAtAnyDepth) {
    GitignoreMatcher matcher("*.log\nnode_modules\n");
    EXPECT_TRUE(matcher.ignores("a.log", false));
    EXPECT_TRUE(matcher.ignores("deep/dir/a.log", false));
    EXPECT_TRUE(matcher.ignores("x/node_modules", true));
    EXPECT_FALSE(matcher.ignores("a.txt", false));
}

TEST(GitignoreMatcherTest, DirectoryOnlyRule) {
    GitignoreMatcher matcher("build/\n");
    EXPECT_TRUE(matcher.ignores("build", true));
    EXPECT_FALSE(matcher.ignores("build", false));
    EXPECT_TRUE(matcher.ignores("build/out.o", false));
}

TEST(GitignoreMatcherTest, AnchoredPattern) {
    GitignoreMatcher matcher("/dist\nsrc/gen\n");
    EXPECT_TRUE(matcher.ignores("dist", true));
    EXPECT_FALSE(matcher.ignores("pkg/dist", true));
    EXPECT_TRUE(matcher.ignores("src/gen", true));
    EXPECT_FALSE(matcher.ignores("other/src/gen", true));
}

TEST(GitignoreMatcherTest, NegationReincludesFile) {
    GitignoreMatcher matcher("*.md\n!README.md\n");
    EXPECT_TRUE(matcher.ignores("notes.md", false));
    EXPECT_FALSE(matcher.ignores("README.md", false));
}

TEST(GitignoreMatcherTest, CommentsAndBlankLinesIgnored) {
    GitignoreMatcher matcher("# comment\n\n   \ntmp\n");
    EXPECT_TRUE(matcher.ignores("tmp", true));
    EXPECT_FALSE(matcher.ignores("comment", false));
}

TEST(GitignoreMatcherTest, ParentExclusionCannotBeUndone) {
    GitignoreMatcher matcher("out/\n!out/keep.txt\n");
    EXPECT_TRUE(matcher.ignores("out/keep.txt", false));
}

TEST(GitignoreMatcherTest, DecideReportsTriState) {
    GitignoreMatcher matcher("*.log\n!keep.log\nbuild/\n");
    EXPECT_EQ(matcher.decide("a.log", false), true);
    EXPECT_EQ(matcher.decide("keep.log", false), false);
    EXPECT_FALSE(matcher.decide("a.txt", false).has_value());
    EXPECT_EQ(matcher.decide("build", true), true);
    EXPECT_FALSE(matcher.decide("build", false).has_value());
    // No parent cascade here: only ignores() walks the directories.
    EXPECT_FALSE(matcher.decide("build/x.txt", false).has_value());
}
