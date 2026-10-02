#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.delta_validator;

class DeltaValidatorTest : public testing::Test {
protected:
    bool op(const std::string& text) {
        return m_validator.validateOp(Json::parse(text)).has_value();
    }

    bool wire(const std::string& text) {
        return m_validator.validateWireOp(Json::parse(text)).has_value();
    }

    DeltaValidator m_validator;
};

TEST_F(DeltaValidatorTest, AcceptsDecodedOperations) {
    for (const char* text : {R"(["r",{"value":1}])", R"(["s",["value"],1])", R"(["d",["value"]])", R"(["a",["value"],"x"])",
                             R"(["t",["value"],2])", R"(["p",["value"],0,0,[]])", R"(["m",["value"],[0]])",
                             R"(["p",[],0,0,[]])", R"(["m",[],[1,0]])"}) {
        EXPECT_TRUE(op(text)) << text;
    }
}

TEST_F(DeltaValidatorTest, RejectsWireOnlyFormsAsDecodedOperations) {
    for (const char* text : {R"(["s",1])", R"(["d"])", R"(["a","x"])", R"(["t",2])", R"(["p",0,0,[]])", R"(["#",0,["value"]])",
                             R"(["s",0,1])"}) {
        EXPECT_FALSE(op(text)) << text;
        EXPECT_TRUE(wire(text)) << text;
    }
}

TEST_F(DeltaValidatorTest, RejectsMalformedOperations) {
    for (const char* text : {R"(["s","value",1])", R"(["m",[],[0,0]])", R"(["ZZZ",["value"],9])", R"(["p",["values"],0,0,"x"])",
                             R"(["t",["value"],-1])", R"(["s",[],1])", R"(["a",["v"],1])", R"({"op":"s"})", "null", "[]",
                             R"(["r"])"}) {
        EXPECT_FALSE(op(text)) << text;
    }
}

TEST_F(DeltaValidatorTest, RejectsUnsafePathSegments) {
    const auto proto = m_validator.validateOp(Json::parse(R"(["s",["constructor","prototype","x"],true])"));
    ASSERT_FALSE(proto.has_value());
    EXPECT_EQ(proto.error().code, "delta_unsafe_path");
    EXPECT_FALSE(op(R"(["s",["__proto__","x"],true])"));
    EXPECT_FALSE(op(R"(["s",["a",-1],true])"));
    EXPECT_FALSE(op(R"(["s",["a",1.5],true])"));
    EXPECT_FALSE(wire(R"(["#",0,["__proto__"]])"));
}

TEST_F(DeltaValidatorTest, ValidatesWireReferencesAndShortForms) {
    EXPECT_TRUE(wire(R"(["s",0,1])"));
    EXPECT_TRUE(wire(R"(["s",["a"],1])"));
    EXPECT_FALSE(wire(R"(["s","a",1])"));
    EXPECT_FALSE(wire(R"(["s",-1,1])"));
    EXPECT_TRUE(wire(R"(["m",[0,1]])"));
    EXPECT_FALSE(wire(R"(["m",[0,0]])"));
    EXPECT_FALSE(wire(R"(["t",-1])"));
    EXPECT_FALSE(wire(R"(["a",1])"));
    EXPECT_FALSE(wire(R"(["#",0])"));
    EXPECT_FALSE(wire(R"(["x"])"));
}

TEST_F(DeltaValidatorTest, DetectsBaseBatches) {
    EXPECT_TRUE(m_validator.isBase(Json::parse(R"([["r",1],["s",["a"],2]])")));
    EXPECT_FALSE(m_validator.isBase(Json::parse(R"([["s",["a"],2]])")));
    EXPECT_FALSE(m_validator.isBase(Json::array()));
}

TEST_F(DeltaValidatorTest, DoesNotInspectPayloads) {
    EXPECT_TRUE(op(R"(["s",["value"],{"__proto__":{"z":1}}])"));
    EXPECT_TRUE(wire(R"(["r",{"constructor":1}])"));
}
