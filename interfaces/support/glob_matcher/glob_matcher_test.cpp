#include <gtest/gtest.h>

import std;
import pi.support.glob_matcher;

class GlobMatcherTest : public testing::Test {
protected:
    GlobMatcher m_glob;
};

TEST_F(GlobMatcherTest, StarStaysInsideSegment) {
    EXPECT_TRUE(m_glob.matches("*.ts", "a.ts"));
    EXPECT_FALSE(m_glob.matches("*.ts", "dir/a.ts"));
    EXPECT_TRUE(m_glob.matches("src/*.ts", "src/a.ts"));
}

TEST_F(GlobMatcherTest, DoubleStarSpansSegments) {
    EXPECT_TRUE(m_glob.matches("**/*.ts", "a/b/c.ts"));
    EXPECT_TRUE(m_glob.matches("**/*.ts", "c.ts"));
    EXPECT_TRUE(m_glob.matches("src/**", "src/a/b"));
    EXPECT_FALSE(m_glob.matches("src/**/x.ts", "lib/x.ts"));
}

TEST_F(GlobMatcherTest, QuestionMarkAndClasses) {
    EXPECT_TRUE(m_glob.matches("a?c", "abc"));
    EXPECT_FALSE(m_glob.matches("a?c", "ac"));
    EXPECT_TRUE(m_glob.matches("[a-c]x", "bx"));
    EXPECT_FALSE(m_glob.matches("[!a-c]x", "bx"));
}

TEST_F(GlobMatcherTest, Braces) {
    EXPECT_TRUE(m_glob.matches("*.{ts,js}", "a.js"));
    EXPECT_FALSE(m_glob.matches("*.{ts,js}", "a.py"));
    EXPECT_TRUE(m_glob.matches("{a,b/{c,d}}/z", "b/d/z"));
}

TEST_F(GlobMatcherTest, NegationAndDotFiles) {
    EXPECT_TRUE(m_glob.matches("!*.ts", "a.js"));
    EXPECT_FALSE(m_glob.matches("!*.ts", "a.ts"));
    EXPECT_TRUE(m_glob.matches("*", ".hidden"));
}

TEST_F(GlobMatcherTest, StarMatchesAnySubstringWithBacktracking) {
    EXPECT_TRUE(m_glob.matches("a*b*c", "aXXbYYc"));
    EXPECT_FALSE(m_glob.matches("a*b*c", "aXXbYY"));
}
