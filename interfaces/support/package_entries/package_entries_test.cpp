#include <gtest/gtest.h>

import std;
import pi.support.package_entries;

class PackageEntriesTest : public testing::Test {
protected:
    PackageSourceParser m_parser{"/home/me"};
    PackageEntries m_entries{m_parser};
};

TEST_F(PackageEntriesTest, ReadsStringAndObjectEntries) {
    const Json packages = Json::parse(R"(["git:github.com/a/b", {"source":"./pkg","autoload":false,"skills":["-x"],"prompts":[]}, 5, {"nosource":1}])");
    const auto read = m_entries.read(packages, "project");
    ASSERT_EQ(read.size(), 2u);
    EXPECT_EQ(read[0].source, "git:github.com/a/b");
    EXPECT_FALSE(read[0].filtered);
    EXPECT_EQ(read[0].scope, "project");
    EXPECT_TRUE(read[1].filtered);
    EXPECT_EQ(read[1].filter.autoload, false);
    EXPECT_EQ(read[1].filter.skills, (std::vector<std::string>{"-x"}));
    ASSERT_TRUE(read[1].filter.prompts.has_value());
    EXPECT_TRUE(read[1].filter.prompts->empty());
    EXPECT_FALSE(read[1].filter.plugins.has_value());
}

TEST_F(PackageEntriesTest, AddingAppendsOnceAndKeepsFilters) {
    Json packages = Json::array();
    const auto first = m_entries.added(packages, "git:github.com/a/b", "/base");
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, Json::parse(R"(["git:github.com/a/b"])"));
    EXPECT_FALSE(m_entries.added(*first, "git:github.com/a/b", "/base").has_value());
    const auto moved = m_entries.added(*first, "git:github.com/a/b@v2", "/base");
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(*moved, Json::parse(R"(["git:github.com/a/b@v2"])"));
    const Json filtered = Json::parse(R"([{"source":"git:github.com/a/b","skills":[]}])");
    const auto kept = m_entries.added(filtered, "git:github.com/a/b@v2", "/base");
    ASSERT_TRUE(kept.has_value());
    EXPECT_EQ((*kept)[0]["source"], "git:github.com/a/b@v2");
    EXPECT_TRUE((*kept)[0].contains("skills"));
}

TEST_F(PackageEntriesTest, LocalPathsAreStoredRelativeToTheScope) {
    const auto added = m_entries.added(Json::array(), "/base/pkgs/a", "/base");
    ASSERT_TRUE(added.has_value());
    EXPECT_EQ(*added, Json::parse(R"(["pkgs/a"])"));
    EXPECT_FALSE(m_entries.added(*added, "pkgs/a", "/base").has_value());
}

TEST_F(PackageEntriesTest, RemovingMatchesByIdentity) {
    const Json packages = Json::parse(R"(["git:github.com/a/b@v1", {"source":"npm:x@1"}, "./p"])");
    const auto removed = m_entries.removed(packages, "https://github.com/a/b", "/base");
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(removed->size(), 2u);
    EXPECT_FALSE(m_entries.removed(packages, "git:github.com/none/x", "/base").has_value());
    const auto object = m_entries.removed(packages, "npm:x", "/base");
    ASSERT_TRUE(object.has_value());
    EXPECT_EQ(object->size(), 2u);
}
