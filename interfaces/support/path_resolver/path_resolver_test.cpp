#include <gtest/gtest.h>

import std;
import pi.support.path_resolver;

class PathResolverTest : public testing::Test {
protected:
    PathResolver m_resolver{"/home/me"};
};

TEST_F(PathResolverTest, ExpandsTildeAtAndFileUrls) {
    EXPECT_EQ(m_resolver.expandPath("~"), "/home/me");
    EXPECT_EQ(m_resolver.expandPath("~/notes.md"), "/home/me/notes.md");
    EXPECT_EQ(m_resolver.expandPath("@src/a.ts"), "src/a.ts");
    EXPECT_EQ(m_resolver.expandPath("file:///tmp/a%20b.txt"), "/tmp/a b.txt");
}

TEST_F(PathResolverTest, NormalizesUnicodeSpaces) {
    EXPECT_EQ(m_resolver.expandPath("a\xC2\xA0" "b"), "a b");
    EXPECT_EQ(m_resolver.expandPath("a\xE2\x80\xAF" "b"), "a b");
}

TEST_F(PathResolverTest, ResolvesRelativeToCwd) {
    EXPECT_EQ(m_resolver.resolveToCwd("a/../b.txt", "/work"), "/work/b.txt");
    EXPECT_EQ(m_resolver.resolveToCwd("/abs/x", "/work"), "/abs/x");
    EXPECT_EQ(m_resolver.resolveToCwd(".", "/work"), "/work");
    EXPECT_EQ(m_resolver.resolveToCwd("dir/", "/work"), "/work/dir");
}

TEST_F(PathResolverTest, MacOsVariants) {
    const auto variants = m_resolver.readPathVariants("/s/Screenshot 2024 at 10.00.01 AM.png");
    ASSERT_FALSE(variants.empty());
    EXPECT_NE(variants[0].find("\xE2\x80\xAF" "AM."), std::string::npos);
    const auto curly = m_resolver.readPathVariants("/s/Capture d'ecran.png");
    ASSERT_EQ(curly.size(), 1U);
    EXPECT_NE(curly[0].find("\xE2\x80\x99"), std::string::npos);
    EXPECT_TRUE(m_resolver.readPathVariants("/plain").empty());
}

TEST_F(PathResolverTest, RelativeTo) {
    EXPECT_EQ(m_resolver.relativeTo("/work/src/a.ts", "/work"), "src/a.ts");
    EXPECT_EQ(m_resolver.relativeTo("/other/a.ts", "/work"), "/other/a.ts");
}
