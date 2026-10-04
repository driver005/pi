#include <gtest/gtest.h>

import std;
import pi.support.header_merger;

TEST(HeaderMergerTest, FindIsCaseInsensitive) {
    HeaderMerger merger;
    const HttpHeaders headers = {{"Content-Type", "json"}};
    EXPECT_EQ(merger.find(headers, "content-type"), "json");
    EXPECT_FALSE(merger.find(headers, "accept").has_value());
}

TEST(HeaderMergerTest, OverridesReplaceAndSuppress) {
    HeaderMerger merger;
    const HttpHeaders defaults = {{"accept", "a"}, {"x-keep", "k"}, {"x-drop", "d"}};
    const auto merged = merger.merge(defaults, std::vector<HeaderMerger::Override>{
                                                   {"Accept", "b"},
                                                   {"x-drop", std::nullopt},
                                                   {"x-new", "n"}});
    ASSERT_EQ(merged.size(), 3U);
    EXPECT_EQ(merger.find(merged, "accept"), "b");
    EXPECT_FALSE(merger.find(merged, "x-drop").has_value());
    EXPECT_EQ(merger.find(merged, "x-new"), "n");
}

TEST(HeaderMergerTest, MapOverrides) {
    HeaderMerger merger;
    const auto merged = merger.merge({{"a", "1"}}, std::map<std::string, std::string>{{"b", "2"}});
    EXPECT_EQ(merger.find(merged, "b"), "2");
}
