#include <gtest/gtest.h>

import std;
import pi.support.json_comment_stripper;

TEST(JsonCommentStripperTest, RemovesLineComments) {
    JsonCommentStripper stripper;
    EXPECT_EQ(stripper.strip("{\n  // note\n  \"a\": 1 // tail\n}"), "{\n  \n  \"a\": 1 \n}");
}

TEST(JsonCommentStripperTest, KeepsSlashesInsideStrings) {
    JsonCommentStripper stripper;
    EXPECT_EQ(stripper.strip(R"({"url":"http://x//y","q":"a\"//b"})"), R"({"url":"http://x//y","q":"a\"//b"})");
}

TEST(JsonCommentStripperTest, RemovesTrailingCommas) {
    JsonCommentStripper stripper;
    EXPECT_EQ(stripper.strip("{\"a\":[1,2,],\"b\":3,}"), "{\"a\":[1,2],\"b\":3}");
    EXPECT_EQ(stripper.strip("[1, \n ]"), "[1 \n ]");
    EXPECT_EQ(stripper.strip(R"({"a":",}"})"), R"({"a":",}"})");
}
