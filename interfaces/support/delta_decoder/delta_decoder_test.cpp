#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.delta_decoder;
import pi.support.delta_encoder;

TEST(DeltaDecoderTest, ExpandsPathIdsAndShortForms) {
    DeltaDecoder decoder;
    const auto first = decoder.decode(Json::parse(R"([["#",3,["a","b"]],["s",3,1],["s",2],["a",3,"x"],["a","y"]])"));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, Json::parse(R"([["s",["a","b"],1],["s",["a","b"],2],["a",["a","b"],"x"],["a",["a","b"],"y"]])"));
}

TEST(DeltaDecoderTest, ShortFormsDoNotCrossBatches) {
    DeltaDecoder decoder;
    ASSERT_TRUE(decoder.decode(Json::parse(R"([["s",["a"],1]])")).has_value());
    EXPECT_FALSE(decoder.decode(Json::parse(R"([["s",2]])")).has_value());
}

TEST(DeltaDecoderTest, RejectsNonArraysAndEmptyPaths) {
    DeltaDecoder decoder;
    EXPECT_FALSE(decoder.decode(Json::object()).has_value());
    EXPECT_FALSE(decoder.decode(Json::parse(R"([["s",[],1]])")).has_value());
    EXPECT_TRUE(decoder.decode(Json::parse(R"([["p",[],0,0,[1]]])")).has_value());
}

TEST(DeltaDecoderTest, RoundTripsWhatTheEncoderProduces) {
    DeltaEncoder encoder;
    DeltaDecoder decoder;
    for (int i = 0; i < 20; ++i) {
        const Json batch = Json::parse(R"([["s",["x"],)" + std::to_string(i) + R"(],["a",["y"],"z"],["a",["y"],"w"]])");
        EXPECT_EQ(*decoder.decode(encoder.encode(batch)), batch);
    }
}
