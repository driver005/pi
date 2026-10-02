#include "interfaces/support/json_reader/json_reader.h"

#include <gtest/gtest.h>

class JsonReaderTest : public testing::Test {
protected:
    Json m_doc = Json::parse(R"({"s":"x","i":5,"d":1.5,"b":true,"arr":[1],"obj":{"k":"v"},"n":null})");
};

TEST_F(JsonReaderTest, RequiredFieldsReadOrReportPath) {
    JsonReader reader(m_doc, "root");
    EXPECT_EQ(*reader.requireString("s"), "x");
    EXPECT_EQ(*reader.requireInt("i"), 5);
    EXPECT_EQ(*reader.requireNumber("d"), 1.5);
    EXPECT_TRUE(*reader.requireBool("b"));
    EXPECT_EQ((*reader.requireArray("arr"))->size(), 1U);
    const auto bad = reader.requireString("i");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().message, "root.i: expected string");
}

TEST_F(JsonReaderTest, OptionalFieldsAreLenient) {
    JsonReader reader(m_doc);
    EXPECT_EQ(reader.optString("s"), std::optional<std::string>("x"));
    EXPECT_EQ(reader.optString("i"), std::nullopt);
    EXPECT_EQ(reader.optString("n"), std::nullopt);
    EXPECT_EQ(reader.optString("missing"), std::nullopt);
    EXPECT_EQ(reader.optInt("d"), std::optional<std::int64_t>(1));
    EXPECT_EQ(reader.optBool("b"), std::optional<bool>(true));
}

TEST_F(JsonReaderTest, ChildCarriesPath) {
    JsonReader reader(m_doc, "root");
    const JsonReader child = reader.child("obj");
    EXPECT_EQ(*child.requireString("k"), "v");
    const auto missing = child.requireString("nope");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message, "root.obj.nope: expected string");
}

TEST_F(JsonReaderTest, NonObjectBehavesAsEmpty) {
    const Json array = Json::array();
    JsonReader reader(array);
    EXPECT_FALSE(reader.isObject());
    EXPECT_FALSE(reader.has("x"));
    EXPECT_FALSE(reader.requireString("x").has_value());
}
