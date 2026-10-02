#include "interfaces/support/schema_validator/schema_validator.h"

#include <gtest/gtest.h>

class SchemaValidatorTest : public testing::Test {
protected:
    SchemaValidator m_validator;
    Json m_schema = Json::parse(R"({
        "type":"object",
        "properties":{
            "path":{"type":"string"},
            "limit":{"type":"integer","minimum":1},
            "flag":{"type":"boolean"},
            "mode":{"enum":["a","b"]},
            "tags":{"type":"array","items":{"type":"string"}}
        },
        "required":["path"]
    })");
};

TEST_F(SchemaValidatorTest, AcceptsValidArguments) {
    const auto result = m_validator.validateArguments(m_schema, Json::parse(R"({"path":"x","limit":3})"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)["limit"], 3);
}

TEST_F(SchemaValidatorTest, CoercesStringsAndNumbers) {
    const auto result = m_validator.validateArguments(
        m_schema, Json::parse(R"({"path":12,"limit":"5","flag":"true"})"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)["path"], "12");
    EXPECT_EQ((*result)["limit"], 5);
    EXPECT_EQ((*result)["flag"], true);
}

TEST_F(SchemaValidatorTest, ReportsMissingRequiredWithPath) {
    const auto result = m_validator.validateArguments(m_schema, Json::parse("{}"));
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("  - path: Expected required property"), std::string::npos);
}

TEST_F(SchemaValidatorTest, ReportsNestedPathsAndBounds) {
    const auto result = m_validator.validateArguments(
        m_schema, Json::parse(R"({"path":"x","limit":0,"tags":["a",{"x":1}],"mode":"z"})"));
    ASSERT_FALSE(result.has_value());
    const std::string& message = result.error().message;
    EXPECT_NE(message.find("limit: Expected number >= 1"), std::string::npos);
    EXPECT_NE(message.find("tags.1: Expected string"), std::string::npos);
    EXPECT_NE(message.find("mode: Expected value in enum"), std::string::npos);
}

TEST_F(SchemaValidatorTest, DropsNullForOptionalNonNullableProperty) {
    const auto result = m_validator.validateArguments(m_schema, Json::parse(R"({"path":"x","limit":null})"));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->contains("limit"));
}

TEST_F(SchemaValidatorTest, UnionCoercionPicksFirstMatchingMember) {
    const Json schema = Json::parse(R"({"anyOf":[{"type":"integer"},{"type":"boolean"}]})");
    const auto result = m_validator.validateArguments(schema, Json("7"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 7);
}

TEST_F(SchemaValidatorTest, AdditionalPropertiesFalseRejectsExtras) {
    const Json schema = Json::parse(R"({"type":"object","properties":{"a":{"type":"string"}},"additionalProperties":false})");
    EXPECT_FALSE(m_validator.matches(schema, Json::parse(R"({"a":"x","b":1})")));
    EXPECT_TRUE(m_validator.matches(schema, Json::parse(R"({"a":"x"})")));
}

TEST_F(SchemaValidatorTest, ResolvesLocalRefs) {
    const Json schema = Json::parse(R"({"$defs":{"n":{"type":"number"}},"type":"object","properties":{"v":{"$ref":"#/$defs/n"}}})");
    EXPECT_TRUE(m_validator.matches(schema, Json::parse(R"({"v":1.5})")));
    EXPECT_FALSE(m_validator.matches(schema, Json::parse(R"({"v":"x"})")));
}

TEST_F(SchemaValidatorTest, StringLengthCountsCodePoints) {
    const Json schema = Json::parse(R"({"type":"string","maxLength":2})");
    EXPECT_TRUE(m_validator.matches(schema, Json("\xc3\xa9\xc3\xa9")));
    EXPECT_FALSE(m_validator.matches(schema, Json("abc")));
}
