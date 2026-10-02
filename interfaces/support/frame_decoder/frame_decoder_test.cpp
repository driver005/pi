#include <gtest/gtest.h>

import std;
import pi.support.frame_decoder;
import pi.support.frame_encoder;

class FrameDecoderTest : public testing::Test {
protected:
    std::string frame(const std::string& payload) {
        return *m_encoder.encode(payload);
    }

    FrameEncoder m_encoder;
};

TEST_F(FrameDecoderTest, DecodesFragmentedCoalescedAndEmptyFramesInOrder) {
    const std::string wire = frame("\x01\x02\x03") + frame("") + frame("\x04");
    const std::vector<std::string> expected{"\x01\x02\x03", "", "\x04"};
    FrameDecoder bytewise;
    std::vector<std::string> frames;
    for (const char byte : wire) {
        const auto pushed = bytewise.push(std::string_view(&byte, 1));
        ASSERT_TRUE(pushed.has_value());
        frames.insert(frames.end(), pushed->begin(), pushed->end());
    }
    EXPECT_TRUE(bytewise.end().has_value());
    EXPECT_EQ(frames, expected);

    FrameDecoder coalesced;
    EXPECT_EQ(*coalesced.push(wire), expected);
    EXPECT_TRUE(coalesced.end().has_value());
}

TEST_F(FrameDecoderTest, AssemblesLargePayloads) {
    std::string payload(70000, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(i % 251);
    }
    const std::string wire = frame(payload);
    FrameDecoder decoder;
    std::vector<std::string> frames;
    for (const std::string_view part : {std::string_view(wire).substr(0, 101), std::string_view(wire).substr(101, 65440),
                                        std::string_view(wire).substr(65541)}) {
        const auto pushed = decoder.push(part);
        ASSERT_TRUE(pushed.has_value());
        frames.insert(frames.end(), pushed->begin(), pushed->end());
    }
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0], payload);
}

TEST_F(FrameDecoderTest, HandlesEverySplitPoint) {
    const std::string wire = frame("\x0A\x14\x1E\x28");
    for (std::size_t split = 0; split <= wire.size(); ++split) {
        FrameDecoder decoder;
        auto first = decoder.push(std::string_view(wire).substr(0, split));
        auto second = decoder.push(std::string_view(wire).substr(split));
        ASSERT_TRUE(first.has_value() && second.has_value());
        first->insert(first->end(), second->begin(), second->end());
        EXPECT_EQ(*first, std::vector<std::string>{"\x0A\x14\x1E\x28"});
        EXPECT_TRUE(decoder.end().has_value());
    }
}

TEST_F(FrameDecoderTest, AcceptsEmptyChunksAndACleanEmptyStream) {
    FrameDecoder decoder;
    EXPECT_TRUE(decoder.push("")->empty());
    EXPECT_TRUE(decoder.end().has_value());
}

TEST_F(FrameDecoderTest, RejectsTruncatedStreams) {
    FrameDecoder header;
    EXPECT_TRUE(header.push(std::string("\0\0\0", 3))->empty());
    EXPECT_FALSE(header.end().has_value());
    FrameDecoder payload;
    EXPECT_TRUE(payload.push(std::string("\0\0\0\2\1", 5))->empty());
    EXPECT_FALSE(payload.end().has_value());
}

TEST_F(FrameDecoderTest, RejectsOversizedLengthsAtTheHeader) {
    FrameDecoder decoder(3);
    const auto result = decoder.push(std::string("\0\0\0\4", 4));
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("limit"), std::string::npos);
    const auto again = decoder.push("\x01");
    ASSERT_FALSE(again.has_value());
    EXPECT_NE(again.error().message.find("failed"), std::string::npos);
}

TEST_F(FrameDecoderTest, AcceptsAFrameAtTheMaximum) {
    FrameDecoder decoder(3);
    EXPECT_EQ(*decoder.push(frame("\x01\x02\x03")), std::vector<std::string>{"\x01\x02\x03"});
    EXPECT_TRUE(decoder.end().has_value());
}

TEST_F(FrameDecoderTest, CannotBePushedAfterEnd) {
    FrameDecoder decoder;
    EXPECT_TRUE(decoder.end().has_value());
    const auto pushed = decoder.push("");
    ASSERT_FALSE(pushed.has_value());
    EXPECT_NE(pushed.error().message.find("ended"), std::string::npos);
    EXPECT_FALSE(decoder.end().has_value());
}
