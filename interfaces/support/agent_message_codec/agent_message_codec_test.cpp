#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.agent_message_codec;

class AgentMessageCodecTest : public testing::Test {
protected:
    AgentMessageCodec m_codec;
};

TEST_F(AgentMessageCodecTest, UserMessageRoundTrip) {
    const Json json = Json::parse(R"({"role":"user","content":[{"type":"text","text":"hi"}],"timestamp":5})");
    const auto message = m_codec.fromJson(json);
    ASSERT_TRUE(message.has_value());
    EXPECT_TRUE(std::holds_alternative<UserMessage>(*message));
    EXPECT_EQ(m_codec.roleOf(*message), "user");
    EXPECT_EQ(m_codec.timestampOf(*message), 5);
    EXPECT_EQ(m_codec.toJson(*message), json);
}

TEST_F(AgentMessageCodecTest, CustomRolesKeepAllFieldsAndOrder) {
    const std::string text = R"({"role":"bashExecution","command":"ls","output":"a","exitCode":0,"cancelled":false,"truncated":false,"timestamp":9})";
    const auto message = m_codec.fromJson(Json::parse(text));
    ASSERT_TRUE(message.has_value());
    const auto* custom = std::get_if<CustomMessage>(&*message);
    ASSERT_NE(custom, nullptr);
    EXPECT_EQ(custom->role, "bashExecution");
    EXPECT_EQ(custom->data["command"], "ls");
    EXPECT_EQ(custom->timestamp, 9);
    EXPECT_EQ(m_codec.toJson(*message).dump(), text);
}

TEST_F(AgentMessageCodecTest, UnknownRoleWithoutTimestamp) {
    const auto message = m_codec.fromJson(Json::parse(R"({"role":"mystery","x":1})"));
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(m_codec.timestampOf(*message), 0);
    EXPECT_EQ(m_codec.toJson(*message)["x"], 1);
}

TEST_F(AgentMessageCodecTest, RejectsNonMessages) {
    EXPECT_FALSE(m_codec.fromJson(Json::parse("[]")).has_value());
    EXPECT_FALSE(m_codec.fromJson(Json::parse(R"({"content":"x"})")).has_value());
}

TEST_F(AgentMessageCodecTest, ListRoundTrip) {
    const Json json = Json::parse(R"([{"role":"user","content":"a","timestamp":1},{"role":"custom","customType":"x","content":"c","display":true,"timestamp":2}])");
    const auto messages = m_codec.listFromJson(json);
    ASSERT_TRUE(messages.has_value());
    ASSERT_EQ(messages->size(), 2U);
    EXPECT_EQ(m_codec.listToJson(*messages), json);
}
