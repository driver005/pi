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

TEST(DeltaDecoderGoldenTest, DecodesLikeTheTypeScriptImplementation) {
    std::ifstream file("src/testing/ts_golden/ts_delta_golden.json");
    ASSERT_TRUE(file.good());
    const Json golden = Json::parse(file);
    int rejected = 0;
    for (const Json& scenario : golden.at("decode")) {
        DeltaDecoder decoder;
        const std::string name = scenario.at("name").get<std::string>();
        const Json& batches = scenario.at("batches");
        const Json& results = scenario.at("results");
        for (std::size_t index = 0; index < results.size(); ++index) {
            const auto decoded = decoder.decode(batches[index]);
            const bool accepted = results[index].at("accepted").get<bool>();
            ASSERT_EQ(decoded.has_value(), accepted) << name << " batch " << index << (decoded ? "" : " " + decoded.error().message);
            if (accepted) {
                EXPECT_EQ(*decoded, results[index].at("ops")) << name << " batch " << index;
            } else {
                ++rejected;
            }
        }
    }
    EXPECT_GT(rejected, 10);
}
