#include <gtest/gtest.h>

import std;
import pi.support.telemetry_schema_validator;

class TelemetrySchemaValidatorTest : public testing::Test {
protected:
    TelemetrySchemaValidator m_validator{Json::parse(R"({
        "version": 1,
        "spans": {
            "agent.run": {
                "description": "A run", "parents": {"kind": "root_or_external"},
                "startAttributes": {"model": {"type": "string", "required": true}, "mode": {"type": "string", "required": false, "values": ["fast", "slow"]}},
                "endAttributes": {"turns": {"type": "number"}, "tags": {"type": "string[]", "elementValues": ["a", "b"]}},
                "events": {"retry": {"description": "r", "attributes": {"attempt": {"type": "number", "required": true}}}},
                "status": {"default": "ok", "errorWhen": "fails"}
            },
            "agent.turn": {
                "description": "A turn", "parents": {"kind": "spans", "spans": ["agent.run"]},
                "startAttributes": {}, "endAttributes": {}, "status": {"default": "ok", "errorWhen": "fails"}
            }
        }})")};
};

TEST_F(TelemetrySchemaValidatorTest, TheSchemaMustBeWellFormed) {
    EXPECT_TRUE(m_validator.validateSchema().has_value());
    EXPECT_FALSE(TelemetrySchemaValidator(Json::array()).validateSchema().has_value());
    EXPECT_FALSE(TelemetrySchemaValidator(Json::parse(R"({"version":1,"spans":{"x":{"description":"d"}}})")).validateSchema().has_value());
    EXPECT_FALSE(TelemetrySchemaValidator(Json::parse(R"({"version":1,"spans":{"x":{"description":"d","parents":{"kind":"odd"},"startAttributes":{},"endAttributes":{}}}})")).validateSchema().has_value());
}

TEST_F(TelemetrySchemaValidatorTest, StartAttributesNeedRequiredOnesAndKnownTypedValues) {
    EXPECT_TRUE(m_validator.validateStart("agent.run", Json{{"model", "m"}, {"mode", "fast"}}).has_value());
    EXPECT_TRUE(m_validator.validateStart("agent.run", Json{{"model", "m"}, {"mode", nullptr}}).has_value());
    EXPECT_FALSE(m_validator.validateStart("agent.run", Json{{"mode", "fast"}}).has_value());
    EXPECT_FALSE(m_validator.validateStart("agent.run", Json{{"model", 3}}).has_value());
    EXPECT_FALSE(m_validator.validateStart("agent.run", Json{{"model", "m"}, {"mode", "medium"}}).has_value());
    EXPECT_FALSE(m_validator.validateStart("agent.run", Json{{"model", "m"}, {"extra", 1}}).has_value());
    EXPECT_FALSE(m_validator.validateStart("nope", Json::object()).has_value());
}

TEST_F(TelemetrySchemaValidatorTest, EndAttributesAreOptionalAndArraysCheckTheirElements) {
    EXPECT_TRUE(m_validator.validateEnd("agent.run", Json::object()).has_value());
    EXPECT_TRUE(m_validator.validateEnd("agent.run", Json{{"turns", 2}, {"tags", Json::array({"a", "b"})}}).has_value());
    EXPECT_FALSE(m_validator.validateEnd("agent.run", Json{{"tags", Json::array({"c"})}}).has_value());
    EXPECT_FALSE(m_validator.validateEnd("agent.run", Json{{"tags", "a"}}).has_value());
}

TEST_F(TelemetrySchemaValidatorTest, EventsAndParents) {
    EXPECT_TRUE(m_validator.validateEvent("agent.run", "retry", Json{{"attempt", 1}}).has_value());
    EXPECT_FALSE(m_validator.validateEvent("agent.run", "retry", Json::object()).has_value());
    EXPECT_FALSE(m_validator.validateEvent("agent.run", "other", Json::object()).has_value());
    EXPECT_TRUE(m_validator.validateParent("agent.run", std::nullopt).has_value());
    EXPECT_FALSE(m_validator.validateParent("agent.run", "agent.turn").has_value());
    EXPECT_TRUE(m_validator.validateParent("agent.turn", "agent.run").has_value());
    EXPECT_FALSE(m_validator.validateParent("agent.turn", std::nullopt).has_value());
}
