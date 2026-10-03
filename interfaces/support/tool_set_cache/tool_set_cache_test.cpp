#include <gtest/gtest.h>

import std;
import pi.support.tool_set_cache;
import pi.testing.scripted_tool;

TEST(ToolSetCacheTest, BuildsEachDirectorysSetOnceAndFindsToolsByName) {
    std::vector<std::string> built;
    ToolSetCache cache([&](const std::string& cwd) {
        built.push_back(cwd);
        return ToolSetCache::ToolSet{std::make_shared<ScriptedTool>("echo", cwd)};
    });
    EXPECT_EQ(cache.tools("/a").size(), 1u);
    EXPECT_EQ(cache.tools("/a").size(), 1u);
    EXPECT_EQ(cache.tools("/b").size(), 1u);
    EXPECT_EQ(built, (std::vector<std::string>{"/a", "/b"}));
    EXPECT_TRUE(cache.find("/a", "echo"));
    EXPECT_FALSE(cache.find("/a", "missing"));
    EXPECT_NE(cache.find("/a", "echo"), cache.find("/b", "echo"));
}
