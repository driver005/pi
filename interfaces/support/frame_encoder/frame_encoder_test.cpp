#include <gtest/gtest.h>

import std;
import pi.support.frame_encoder;

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
