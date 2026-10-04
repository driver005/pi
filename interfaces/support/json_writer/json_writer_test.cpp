#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.json_writer;

TEST(JsonWriterTest, CompactKeepsInsertionOrder) {
    JsonWriter writer;
    Json value = Json::object();
    value["z"] = 1;
    value["a"] = 2;
    EXPECT_EQ(writer.compact(value), R"({"z":1,"a":2})");
}

TEST(JsonWriterTest, InvalidUtf8IsReplaced) {
    JsonWriter writer;
    Json value = Json::object();
    value["text"] = std::string("ok\xff\xfe!");
    const std::string out = writer.compact(value);
    EXPECT_NE(out.find("\xEF\xBF\xBD"), std::string::npos);
    EXPECT_NE(out.find("ok"), std::string::npos);
}

TEST(JsonWriterTest, PrettyIndents) {
    JsonWriter writer;
    Json value = Json::object();
    value["a"] = 1;
    EXPECT_EQ(writer.pretty(value, 2), "{\n  \"a\": 1\n}");
}
