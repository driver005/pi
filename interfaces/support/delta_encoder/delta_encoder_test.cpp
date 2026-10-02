#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.delta_decoder;
import pi.support.delta_encoder;

class DeltaCodecTest : public testing::Test {
protected:
    Json j(const std::string& text) {
        return Json::parse(text);
    }

    DeltaEncoder m_encoder;
    DeltaDecoder m_decoder;
};

TEST_F(DeltaCodecTest, InternsPathsOmitsAdjacentPathsAndRoundTrips) {
    const Json first = j(R"([["t",["nested","text"],1],["a",["nested","text"],"x"]])");
    EXPECT_EQ(*m_decoder.decode(m_encoder.encode(first)), first);
    const Json second = j(R"([["a",["nested","text"],"y"]])");
    const Json wire = m_encoder.encode(second);
    EXPECT_EQ(wire, j(R"([["#",0,["nested","text"]],["a",0,"y"]])"));
    EXPECT_EQ(*m_decoder.decode(wire), second);
}

TEST_F(DeltaCodecTest, ResetsPathDictionariesOnABase) {
    m_encoder.encode(j(R"([["s",["value"],1]])"));
    m_encoder.encode(j(R"([["s",["value"],2]])"));
    EXPECT_EQ(m_encoder.encode(j(R"([["r",{"value":3}]])")), j(R"([["r",{"value":3}]])"));
    EXPECT_EQ(m_encoder.encode(j(R"([["s",["value"],4]])")), j(R"([["s",["value"],4]])"));
}

TEST_F(DeltaCodecTest, OmitsAnAdjacentRepeatedPath) {
    EXPECT_EQ(m_encoder.encode(j(R"([["s",["value"],1],["s",["value"],2]])")), j(R"([["s",["value"],1],["s",2]])"));
}

TEST_F(DeltaCodecTest, InternsOnSecondUseRatherThanFirst) {
    EXPECT_EQ(m_encoder.encode(j(R"([["a",["a","deep"],"1"]])")), j(R"([["a",["a","deep"],"1"]])"));
    EXPECT_EQ(m_encoder.encode(j(R"([["a",["a","deep"],"2"]])")), j(R"([["#",0,["a","deep"]],["a",0,"2"]])"));
}

TEST_F(DeltaCodecTest, ShortensEveryVerb) {
    const Json ops = j(R"([["d",["a"]],["d",["a"]],["t",["b"],1],["t",["b"],2],["p",["c"],0,0,[1]],["p",["c"],1,0,[2]],["m",["e"],[1,0]],["m",["e"],[0,1]]])");
    const Json wire = m_encoder.encode(ops);
    EXPECT_EQ(wire, j(R"([["d",["a"]],["d"],["t",["b"],1],["t",2],["p",["c"],0,0,[1]],["p",1,0,[2]],["m",["e"],[1,0]],["m",[0,1]]])"));
    EXPECT_EQ(*m_decoder.decode(wire), ops);
}

TEST_F(DeltaCodecTest, RejectsUnresolvedShortFormsAndUnsafeInternedPaths) {
    EXPECT_FALSE(DeltaDecoder().decode(j(R"([["a","x"]])")).has_value());
    EXPECT_FALSE(DeltaDecoder().decode(j(R"([["#",0,["__proto__"]],["s",0,true]])")).has_value());
    EXPECT_FALSE(DeltaDecoder().decode(j(R"([["#",0,["__proto__","w"]],["s",0,true]])")).has_value());
    EXPECT_FALSE(DeltaDecoder().decode(j(R"([["t",["value"],-1]])")).has_value());
    EXPECT_FALSE(DeltaDecoder().decode(j(R"([["s",7,1]])")).has_value());
}

TEST_F(DeltaCodecTest, DoesNotCollidePathsContainingNullCharacters) {
    const Json ops = j(R"([["s",["a\u0000b"],1],["s",["a","b"],2]])");
    EXPECT_EQ(*DeltaDecoder().decode(DeltaEncoder().encode(ops)), ops);
}

TEST_F(DeltaCodecTest, ClearsDecoderIdsOnABaseBatch) {
    DeltaDecoder decoder;
    ASSERT_TRUE(decoder.decode(j(R"([["#",0,["a"]],["a",0,"1"]])")).has_value());
    ASSERT_TRUE(decoder.decode(j(R"([["r",{"a":""}]])")).has_value());
    EXPECT_FALSE(decoder.decode(j(R"([["a",0,"2"]])")).has_value());
}

TEST_F(DeltaCodecTest, MakesBatchesAfterABaseSelfContained) {
    const Json path = j(R"(["a","deep"])");
    m_encoder.encode(Json::array({Json::array({"a", path, "1"})}));
    m_encoder.encode(Json::array({Json::array({"a", path, "2"})}));
    const Json base = m_encoder.encode(j(R"([["r",{"a":{"deep":"x"}}]])"));
    const Json after = m_encoder.encode(Json::array({Json::array({"a", path, "3"})}));
    EXPECT_EQ(after, Json::array({Json::array({"a", path, "3"})}));
    DeltaDecoder fresh;
    EXPECT_TRUE(fresh.decode(base).has_value());
    EXPECT_EQ(*fresh.decode(after), after);
}

TEST_F(DeltaCodecTest, RoundTripsDeterministicMixedStreams) {
    for (int i = 0; i < 100; ++i) {
        const Json batch = Json::array({Json::array({"s", Json::array({"rows", i, "value"}), i}),
                                        Json::array({"a", Json::array({"output"}), std::to_string(i)}),
                                        Json::array({"p", Json::array({"tail"}), i, 0, Json::array({i})})});
        EXPECT_EQ(*m_decoder.decode(m_encoder.encode(batch)), batch) << i;
    }
}
