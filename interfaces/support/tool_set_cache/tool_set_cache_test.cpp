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

TEST(ToolSetCacheTest, InvalidatingDropsASetAndTellsTheListenersUntilTheyLeave) {
    int built = 0;
    ToolSetCache cache([&](const std::string&) {
        ++built;
        return ToolSetCache::ToolSet{std::make_shared<ScriptedTool>("echo", "x")};
    });
    std::vector<std::string> told;
    const std::int64_t id = cache.subscribe([&](const std::string& cwd) { told.push_back(cwd); });
    const auto first = cache.find("/a", "echo");
    cache.invalidate("/a");
    EXPECT_EQ(told, std::vector<std::string>{"/a"});
    EXPECT_NE(cache.find("/a", "echo"), first);
    EXPECT_EQ(built, 2);
    cache.unsubscribe(id);
    cache.invalidate("/a");
    EXPECT_EQ(told.size(), 1u);
}

TEST(ToolSetCacheTest, TheFactoryMayCallBackIntoTheCache) {
    ToolSetCache* self = nullptr;
    ToolSetCache cache([&](const std::string& cwd) {
        if (cwd == "/outer") {
            self->tools("/inner");
        }
        return ToolSetCache::ToolSet{};
    });
    self = &cache;
    EXPECT_TRUE(cache.tools("/outer").empty());
}
