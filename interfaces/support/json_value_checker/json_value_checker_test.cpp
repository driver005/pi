#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.json_value_checker;

TEST(JsonValueCheckerTest, AcceptsStrictJson) {
    const JsonValueChecker checker;
    EXPECT_TRUE(checker.valid(nullptr));
    EXPECT_TRUE(checker.valid(Json{{"a", Json::array({1, 2.5, "x", true, nullptr})}}));
}

TEST(JsonValueCheckerTest, RejectsBinaryAndNonFiniteNumbers) {
    const JsonValueChecker checker;
    EXPECT_FALSE(checker.valid(Json::binary({1})));
    EXPECT_FALSE(checker.valid(Json::array({Json::binary({1})})));
    EXPECT_FALSE(checker.valid(std::numeric_limits<double>::quiet_NaN()));
    EXPECT_FALSE(checker.valid(Json{{"n", std::numeric_limits<double>::infinity()}}));
    EXPECT_FALSE(checker.valid(Json::value_t::discarded));
}
