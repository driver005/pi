#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.protocol_message_decoder;

class ProtocolMessageDecoderTest : public testing::Test {
protected:
    std::string frame(ProtocolSide side, const Json& message) {
        return *m_codec.encode(side, message);
    }

    const Json m_hello{{"type", "hello"}, {"version", 8}};
    const Json m_request{{"type", "request"},
                         {"id", "request-1"},
                         {"target", Json{{"serverId", "00000000-0000-4000-8000-000000000001"}}},
                         {"call", Json{{"serviceId", "pi.session-directory"}, {"member", "list"}, {"args", Json::array()}}}};
    ProtocolCodec m_codec;
};

TEST_F(ProtocolMessageDecoderTest, DecodesFragmentedAndCoalescedClientMessages) {
    const std::string wire = frame(ProtocolSide::Client, m_hello) + frame(ProtocolSide::Client, m_request);
    for (std::size_t split = 0; split <= wire.size(); ++split) {
        ProtocolMessageDecoder decoder(ProtocolSide::Client);
        auto first = decoder.push(std::string_view(wire).substr(0, split));
        auto second = decoder.push(std::string_view(wire).substr(split));
        ASSERT_TRUE(first.has_value() && second.has_value());
        first->insert(first->end(), second->begin(), second->end());
        EXPECT_EQ(*first, (std::vector<Json>{m_hello, m_request}));
        EXPECT_TRUE(decoder.end().has_value());
    }
}

TEST_F(ProtocolMessageDecoderTest, DecodesServerMessages) {
    const Json hello{{"type", "hello"}, {"version", 8}, {"serverId", "00000000-0000-4000-8000-000000000001"}};
    const Json response{{"type", "response"}, {"id", "request-1"}, {"ok", true}, {"result", Json::array()}};
    const std::string wire = frame(ProtocolSide::Server, hello) + frame(ProtocolSide::Server, response);
    const std::size_t split = frame(ProtocolSide::Server, hello).size() + frame(ProtocolSide::Server, response).size() / 2;
    ProtocolMessageDecoder decoder(ProtocolSide::Server);
    EXPECT_EQ(*decoder.push(std::string_view(wire).substr(0, split)), std::vector<Json>{hello});
    EXPECT_EQ(*decoder.push(std::string_view(wire).substr(split)), std::vector<Json>{response});
    EXPECT_TRUE(decoder.end().has_value());
}

TEST_F(ProtocolMessageDecoderTest, InvalidInputIsFinal) {
    const CborEncoder encoder;
    const FrameEncoder frames;
    const std::vector<std::string> bad{*frames.encode(""), *frames.encode(std::string("\xFF")),
                                       *frames.encode(*encoder.encode(Json{{"type", "hello"}, {"version", 1}, {"extra", true}}))};
    for (const std::string& wire : bad) {
        ProtocolMessageDecoder decoder(ProtocolSide::Client);
        EXPECT_FALSE(decoder.push(wire).has_value());
        const auto again = decoder.push(frame(ProtocolSide::Client, m_hello));
        ASSERT_FALSE(again.has_value());
        EXPECT_NE(again.error().message.find("failed"), std::string::npos);
    }
}

TEST_F(ProtocolMessageDecoderTest, RejectsTruncatedAndOversizedFraming) {
    ProtocolMessageDecoder truncated(ProtocolSide::Server);
    EXPECT_TRUE(truncated.push(std::string("\0\0\0\2\1", 5))->empty());
    EXPECT_FALSE(truncated.end().has_value());
    ProtocolMessageDecoder oversized(ProtocolSide::Client, 3);
    EXPECT_FALSE(oversized.push(std::string("\0\0\0\4", 4)).has_value());
}
