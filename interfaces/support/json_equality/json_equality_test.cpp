#include <gtest/gtest.h>

import std;
import pi.support.json_equality;

TEST(JsonEqualityTest, IgnoresObjectKeyOrderAtAnyDepth) {
    JsonEquality equality;
    EXPECT_TRUE(equality.equal(Json::parse(R"({"a":1,"b":{"x":[1,{"p":1,"q":2}],"y":null}})"),
                               Json::parse(R"({"b":{"y":null,"x":[1,{"q":2,"p":1}]},"a":1})")));
}

TEST(JsonEqualityTest, ArrayOrderAndValuesMatter) {
    JsonEquality equality;
    EXPECT_FALSE(equality.equal(Json::parse("[1,2]"), Json::parse("[2,1]")));
    EXPECT_FALSE(equality.equal(Json::parse(R"({"a":1})"), Json::parse(R"({"a":2})")));
    EXPECT_FALSE(equality.equal(Json::parse(R"({"a":1})"), Json::parse(R"({"a":1,"b":2})")));
    EXPECT_TRUE(equality.equal(Json(3), Json(3)));
}
