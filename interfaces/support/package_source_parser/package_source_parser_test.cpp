#include <gtest/gtest.h>

import std;
import pi.support.package_source_parser;

class PackageSourceParserTest : public testing::Test {
protected:
    PackageSourceParser m_parser{"/home/me"};
};

TEST_F(PackageSourceParserTest, DetectsTheSourceType) {
    EXPECT_EQ(m_parser.parse("npm:@scope/tool@1.0.0").type, "npm");
    EXPECT_EQ(m_parser.parse("npm:@scope/tool@1.0.0").npmSpec, "@scope/tool@1.0.0");
    EXPECT_EQ(m_parser.parse("git:github.com/a/b@v1").type, "git");
    EXPECT_EQ(m_parser.parse("github:a/b").path, "a/b");
    EXPECT_EQ(m_parser.parse("github:a/b").host, "github.com");
    EXPECT_EQ(m_parser.parse("https://example.com/a/b").type, "git");
    EXPECT_EQ(m_parser.parse("./pkg").type, "local");
    EXPECT_EQ(m_parser.parse("a/b").type, "local");
    EXPECT_EQ(m_parser.parse("  ~/pkg ").localPath, "~/pkg");
}

TEST_F(PackageSourceParserTest, IdentitiesIgnoreVersionsAndRefs) {
    EXPECT_EQ(m_parser.identity("npm:@scope/tool@1.0.0", "/base"), "npm:@scope/tool");
    EXPECT_EQ(m_parser.identity("npm:tool", "/base"), "npm:tool");
    EXPECT_EQ(m_parser.identity("git:github.com/a/b@v1", "/base"), "git:github.com/a/b");
    EXPECT_EQ(m_parser.identity("https://github.com/a/b.git", "/base"), "git:github.com/a/b");
    EXPECT_EQ(m_parser.identity("../pkg", "/base/x"), "local:/base/pkg");
    EXPECT_EQ(m_parser.identity("~/pkg", "/base"), "local:/home/me/pkg");
}

TEST_F(PackageSourceParserTest, LocalSourcesAreStoredRelativeToTheScope) {
    EXPECT_EQ(m_parser.settingsForm("/base/pkgs/a", "/base"), "pkgs/a");
    EXPECT_EQ(m_parser.settingsForm("/base", "/base"), ".");
    EXPECT_EQ(m_parser.settingsForm("git:github.com/a/b", "/base"), "git:github.com/a/b");
}
