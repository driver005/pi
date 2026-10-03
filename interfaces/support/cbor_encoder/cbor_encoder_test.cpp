#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.cbor_decoder;
import pi.support.cbor_encoder;

class CborEncoderTest : public testing::Test {
protected:
    std::string hex(const std::string& bytes) {
        std::string out;
        for (const char c : bytes) {
            out += std::format("{:02x}", static_cast<unsigned char>(c));
        }
        return out;
    }

    std::string encoded(const Json& value) {
        const auto result = m_encoder.encode(value);
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
        return result ? hex(*result) : "";
    }

    CborEncoder m_encoder;
};

TEST_F(CborEncoderTest, MatchesTheRfc8949Vectors) {
    EXPECT_EQ(encoded(nullptr), "f6");
    EXPECT_EQ(encoded(false), "f4");
    EXPECT_EQ(encoded(true), "f5");
    EXPECT_EQ(encoded(0), "00");
    EXPECT_EQ(encoded(23), "17");
    EXPECT_EQ(encoded(24), "1818");
    EXPECT_EQ(encoded(100), "1864");
    EXPECT_EQ(encoded(1000), "1903e8");
    EXPECT_EQ(encoded(1000000), "1a000f4240");
    EXPECT_EQ(encoded(1000000000000LL), "1b000000e8d4a51000");
    EXPECT_EQ(encoded(9007199254740991LL), "1b001fffffffffffff");
    EXPECT_EQ(encoded(-1), "20");
    EXPECT_EQ(encoded(-25), "3818");
    EXPECT_EQ(encoded(-1000), "3903e7");
    EXPECT_EQ(encoded(-9007199254740991LL), "3b001ffffffffffffe");
    EXPECT_EQ(encoded(1.1), "fb3ff199999999999a");
    EXPECT_EQ(encoded(-0.0), "fb8000000000000000");
    EXPECT_EQ(encoded(Json::binary({1, 2, 3, 4})), "4401020304");
    EXPECT_EQ(encoded(""), "60");
    EXPECT_EQ(encoded("IETF"), "6449455446");
    EXPECT_EQ(encoded("\xC3\xBC"), "62c3bc");
    EXPECT_EQ(encoded("\xF0\x90\x85\x91"), "64f0908591");
    EXPECT_EQ(encoded(Json::array()), "80");
    EXPECT_EQ(encoded(Json::array({1, 2, 3})), "83010203");
    EXPECT_EQ(encoded(Json::array({1, Json::array({2, 3}), Json::array({4, 5})})), "8301820203820405");
    EXPECT_EQ(encoded(Json{{"a", 1}, {"b", Json::array({2, 3})}}), "a26161016162820203");
}

TEST_F(CborEncoderTest, IntegerValuedFloatsEncodeAsIntegers) {
    EXPECT_EQ(encoded(2.0), "02");
    EXPECT_EQ(encoded(-3.0), "22");
    EXPECT_EQ(encoded(1e3), "1903e8");
}

TEST_F(CborEncoderTest, RejectsUnsupportedValues) {
    EXPECT_FALSE(m_encoder.encode(std::nan("")).has_value());
    EXPECT_FALSE(m_encoder.encode(std::numeric_limits<double>::infinity()).has_value());
    EXPECT_FALSE(m_encoder.encode(Json(9007199254740992LL)).has_value());
    EXPECT_FALSE(m_encoder.encode(Json(-9007199254740992LL)).has_value());
    EXPECT_FALSE(m_encoder.encode(Json(std::uint64_t{18446744073709551615ULL})).has_value());
    EXPECT_FALSE(m_encoder.encode(Json(1e300)).has_value());
}

TEST_F(CborEncoderTest, RejectsInvalidTextAndExcessiveDepth) {
    const auto text = m_encoder.encode(Json(std::string("\xED\xA0\x80")));
    ASSERT_FALSE(text.has_value());
    EXPECT_NE(text.error().message.find("Unicode"), std::string::npos);

    Json deep = nullptr;
    for (int i = 0; i <= kDefaultMaxCborDepth; ++i) {
        deep = Json::array({deep});
    }
    const auto result = m_encoder.encode(deep);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("depth"), std::string::npos);
    Json fits = nullptr;
    for (int i = 0; i < kDefaultMaxCborDepth; ++i) {
        fits = Json::array({fits});
    }
    EXPECT_TRUE(m_encoder.encode(fits).has_value());
}

TEST_F(CborEncoderTest, HonoursCallerLimits) {
    CborOptions containers;
    containers.maxContainerLength = 2;
    EXPECT_FALSE(m_encoder.encode(Json::array({1, 2, 3}), containers).has_value());
    EXPECT_FALSE(m_encoder.encode(Json{{"a", 1}, {"b", 2}, {"c", 3}}, containers).has_value());
    CborOptions bytes;
    bytes.maxByteLength = 2;
    EXPECT_FALSE(m_encoder.encode("ab", bytes).has_value());
    EXPECT_TRUE(m_encoder.encode(Json::array({1}), bytes).has_value());
}

TEST_F(CborEncoderTest, RoundTripsThroughTheDecoder) {
    const CborDecoder decoder;
    const Json value{{"type", "request"}, {"id", "r1"}, {"n", -42}, {"f", 0.5}, {"list", Json::array({true, nullptr, "x"})},
                     {"nested", Json{{"deep", Json::array({Json::array({1})})}}}};
    const auto bytes = m_encoder.encode(value);
    ASSERT_TRUE(bytes.has_value());
    const auto decoded = decoder.decode(*bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, value);
    EXPECT_EQ(decoded->dump(), value.dump());
}

TEST_F(CborEncoderTest, EncodesLikeTheTypeScriptImplementation) {
    std::ifstream file("src/testing/ts_golden/ts_cbor_golden.json");
    ASSERT_TRUE(file.good());
    const Json golden = Json::parse(file);
    for (const Json& vector : golden.at("encode")) {
        const std::string name = vector.at("name").get<std::string>();
        const Json value = Json::parse(vector.at("json").get<std::string>());
        const auto result = m_encoder.encode(value);
        if (vector.at("accepted").get<bool>()) {
            ASSERT_TRUE(result.has_value()) << name << ": " << (result ? "" : result.error().message);
            EXPECT_EQ(hex(*result), vector.at("cbor").get<std::string>()) << name;
        } else {
            EXPECT_FALSE(result.has_value()) << name << " must be rejected like TypeScript does (" << vector.at("error").get<std::string>() << ")";
        }
    }
}
