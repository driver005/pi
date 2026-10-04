#include <gtest/gtest.h>

import std;
import pi.support.resource_set_cache;

TEST(ResourceSetCacheTest, LoadsEachDirectorysResourcesOnce) {
    int loads = 0;
    ResourceSetCache cache([&](const std::string& cwd) {
        ++loads;
        LoadedResources resources;
        resources.systemPrompt = cwd;
        return resources;
    });
    EXPECT_EQ(cache.resources("/a").systemPrompt, "/a");
    EXPECT_EQ(cache.resources("/a").systemPrompt, "/a");
    EXPECT_EQ(cache.resources("/b").systemPrompt, "/b");
    EXPECT_EQ(loads, 2);
}
