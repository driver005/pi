#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

import std;
import pi.support.frame_encoder;
import pi.types.json;

TEST(FrameEncoderTest, PrefixesPayloadsWithAFourByteBigEndianLength) {
    const FrameEncoder encoder;
    EXPECT_EQ(*encoder.encode(std::string("\xAA\xBB\xCC", 3)), std::string("\x00\x00\x00\x03\xAA\xBB\xCC", 7));
    EXPECT_EQ(*encoder.encode(""), std::string("\x00\x00\x00\x00", 4));
    const std::string big(0x10203, 'x');
    const auto frame = encoder.encode(big);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->substr(0, 4), std::string("\x00\x01\x02\x03", 4));
    EXPECT_EQ(frame->size(), 4 + big.size());
}

TEST(FrameEncoderTest, FramesLikeTheTypeScriptImplementation) {
    std::ifstream file("src/testing/ts_golden/ts_cbor_golden.json");
    ASSERT_TRUE(file.good());
    const Json golden = Json::parse(file);
    const auto fromHex = [](const std::string& text) {
        std::string out;
        for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
            out.push_back(static_cast<char>(std::stoi(text.substr(i, 2), nullptr, 16)));
        }
        return out;
    };
    const FrameEncoder encoder;
    int checked = 0;
    for (const Json& vector : golden.at("encode")) {
        if (!vector.at("accepted").get<bool>()) {
            continue;
        }
        const auto frame = encoder.encode(fromHex(vector.at("cbor").get<std::string>()));
        ASSERT_TRUE(frame.has_value());
        EXPECT_EQ(*frame, fromHex(vector.at("frame").get<std::string>())) << vector.at("name").get<std::string>();
        ++checked;
    }
    EXPECT_GT(checked, 40);
}
