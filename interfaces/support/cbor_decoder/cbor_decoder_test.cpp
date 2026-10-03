#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.cbor_decoder;
import pi.support.cbor_encoder;

class CborDecoderTest : public testing::Test {
protected:
    std::string fromHex(const std::string& hex) {
        std::string out;
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
            out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
        }
        return out;
    }

    Result<Json> decode(const std::string& hex) {
        return m_decoder.decode(fromHex(hex));
    }

    CborDecoder m_decoder;
};

TEST_F(CborDecoderTest, DecodesTheRfc8949Vectors) {
    EXPECT_EQ(*decode("f6"), Json(nullptr));
    EXPECT_EQ(*decode("f4"), Json(false));
    EXPECT_EQ(*decode("f5"), Json(true));
    EXPECT_EQ(*decode("1864"), Json(100));
    EXPECT_EQ(*decode("1b000000e8d4a51000"), Json(1000000000000LL));
    EXPECT_EQ(*decode("1b001fffffffffffff"), Json(9007199254740991LL));
    EXPECT_EQ(*decode("3903e7"), Json(-1000));
    EXPECT_EQ(*decode("3b001ffffffffffffe"), Json(-9007199254740991LL));
    EXPECT_EQ(*decode("fb3ff199999999999a"), Json(1.1));
    EXPECT_TRUE(std::signbit(decode("fb8000000000000000")->get<double>()));
    EXPECT_EQ(*decode("4401020304"), Json::binary({1, 2, 3, 4}));
    EXPECT_EQ(*decode("6449455446"), Json("IETF"));
    EXPECT_EQ(*decode("63e6b0b4"), Json("\xE6\xB0\xB4"));
    EXPECT_EQ(*decode("8301820203820405"), Json::array({1, Json::array({2, 3}), Json::array({4, 5})}));
    EXPECT_EQ(*decode("a26161016162820203"), (Json{{"a", 1}, {"b", Json::array({2, 3})}}));
}

TEST_F(CborDecoderTest, IntegerValuedFloatsBecomeIntegers) {
    EXPECT_TRUE(decode("fb3ff0000000000000")->is_number_integer());
    EXPECT_EQ(*decode("fb4000000000000000"), Json(2));
}

TEST_F(CborDecoderTest, PreservesBomAndTreatsProtoAsData) {
    EXPECT_EQ(decode("63efbbbf")->get<std::string>(), "\xEF\xBB\xBF");
    const auto map = decode("a169" "5f5f70726f746f5f5f" "6473616665");
    ASSERT_TRUE(map.has_value());
    EXPECT_EQ((*map)["__proto__"], "safe");
}

TEST_F(CborDecoderTest, RejectsInvalidInput) {
    const std::vector<std::pair<std::string, std::string>> cases{
        {"empty input", ""},
        {"truncated integer", "18"},
        {"reserved additional information", "1c"},
        {"indefinite byte string", "5f"},
        {"indefinite text string", "7f"},
        {"indefinite array", "9f"},
        {"indefinite map", "bf"},
        {"tag", "c000"},
        {"undefined", "f7"},
        {"unsupported simple value", "e0"},
        {"break", "ff"},
        {"float16", "f93c00"},
        {"float32", "fa3f800000"},
        {"positive infinity", "fb7ff0000000000000"},
        {"NaN", "fb7ff8000000000000"},
        {"truncated float64", "fb3ff00000"},
        {"truncated byte string", "44010203"},
        {"truncated text string", "636162"},
        {"truncated array", "8201"},
        {"truncated map", "a16161"},
        {"trailing data", "0000"},
        {"non-string map key", "a10102"},
        {"duplicate map key", "a2616101616102"},
        {"invalid UTF-8 byte", "61ff"},
        {"overlong UTF-8", "62c080"},
        {"UTF-8 surrogate", "63eda080"},
        {"unsafe positive integer", "1b0020000000000000"},
        {"unsafe negative integer", "3b001fffffffffffff"},
        {"unsafe integer encoded as float64", "fb4340000000000000"},
    };
    for (const auto& [label, hex] : cases) {
        EXPECT_FALSE(decode(hex).has_value()) << label;
    }
}

TEST_F(CborDecoderTest, EnforcesDepthAndDeclaredLengthLimits) {
    std::string tooDeep(kDefaultMaxCborDepth + 1, '\x81');
    tooDeep.push_back('\xF6');
    const auto deep = m_decoder.decode(tooDeep);
    ASSERT_FALSE(deep.has_value());
    EXPECT_NE(deep.error().message.find("depth"), std::string::npos);

    for (const char* prefix : {"5a", "7a"}) {
        const auto result = decode(std::string(prefix) + "01000001");
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().message.find("limit"), std::string::npos);
    }
    for (const char* prefix : {"9a", "ba"}) {
        const auto result = decode(std::string(prefix) + "000f4241");
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().message.find("limit"), std::string::npos);
    }
}

TEST_F(CborDecoderTest, SupportsStricterCallerLimits) {
    CborOptions containers;
    containers.maxContainerLength = 2;
    EXPECT_FALSE(m_decoder.decode(fromHex("83010203"), containers).has_value());
    CborOptions bytes;
    bytes.maxByteLength = 2;
    EXPECT_FALSE(m_decoder.decode(fromHex("626162"), bytes).has_value());
}

TEST_F(CborDecoderTest, AcceptsAndRejectsLikeTheTypeScriptImplementation) {
    std::ifstream file("src/testing/ts_golden/ts_cbor_golden.json");
    ASSERT_TRUE(file.good());
    const Json golden = Json::parse(file);
    for (const Json& vector : golden.at("decode")) {
        const std::string name = vector.at("name").get<std::string>();
        const auto result = decode(vector.at("hex").get<std::string>());
        ASSERT_EQ(result.has_value(), vector.at("accepted").get<bool>()) << name << ": " << (result ? "accepted" : result.error().message) << " (TypeScript: "
                                                                          << (vector.at("accepted").get<bool>() ? "accepted" : vector.at("error").get<std::string>()) << ")";
        if (!result) {
            continue;
        }
        if (result->is_binary()) {
            // TypeScript returns a Uint8Array, which its JSON form shows as an index map.
            EXPECT_EQ(result->get_binary().size(), Json::parse(vector.at("json").get<std::string>()).size()) << name;
            continue;
        }
        EXPECT_EQ(*result, Json::parse(vector.at("json").get<std::string>())) << name;
    }
}
