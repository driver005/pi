#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.protocol_codec;

class ProtocolCodecTest : public testing::Test {
protected:
    const Json m_clientHello{{"type", "hello"}, {"version", 8}};
    const Json m_serverHello{{"type", "hello"}, {"version", 8}, {"serverId", "00000000-0000-4000-8000-000000000001"}};
    ProtocolCodec m_codec;
};

TEST_F(ProtocolCodecTest, EncodesCompleteFramesThatDecodeBack) {
    const auto frame = m_codec.encode(ProtocolSide::Client, m_clientHello);
    ASSERT_TRUE(frame.has_value());
    FrameDecoder frames;
    const auto payloads = frames.push(*frame);
    ASSERT_TRUE(payloads.has_value());
    ASSERT_EQ(payloads->size(), 1U);
    const auto decoded = m_codec.decode(ProtocolSide::Client, (*payloads)[0]);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, m_clientHello);
    EXPECT_EQ(m_codec.decode(ProtocolSide::Server, *m_codec.encode(ProtocolSide::Server, m_serverHello).and_then([](const std::string& f) -> Result<std::string> { return f.substr(4); })), m_serverHello);
}

TEST_F(ProtocolCodecTest, RefusesToEncodeInvalidMessages) {
    const auto result = m_codec.encode(ProtocolSide::Client, Json{{"type", "hello"}, {"version", "x"}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "protocol_validation");
    EXPECT_EQ(result.error().message, "Invalid client protocol message");
}

TEST_F(ProtocolCodecTest, EnforcesOutboundFrameLimits) {
    const auto client = m_codec.encode(ProtocolSide::Client, m_clientHello, 8);
    ASSERT_FALSE(client.has_value());
    EXPECT_NE(client.error().message.find("Unable to encode client protocol message"), std::string::npos);
    EXPECT_FALSE(m_codec.encode(ProtocolSide::Server, m_serverHello, 8).has_value());
}

TEST_F(ProtocolCodecTest, RejectsInvalidPayloads) {
    const auto malformed = m_codec.decode(ProtocolSide::Client, std::string("\xFF"));
    ASSERT_FALSE(malformed.has_value());
    EXPECT_NE(malformed.error().message.find("Invalid client protocol frame"), std::string::npos);
    EXPECT_FALSE(m_codec.decode(ProtocolSide::Client, "").has_value());
    const CborEncoder encoder;
    const auto invalid = encoder.encode(Json{{"type", "hello"}, {"version", 1}, {"extra", true}});
    EXPECT_EQ(m_codec.decode(ProtocolSide::Client, *invalid).error().message, "Invalid client protocol message");
}
