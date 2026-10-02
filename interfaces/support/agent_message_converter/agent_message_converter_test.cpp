#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.agent_message_converter;

class AgentMessageConverterTest : public testing::Test {
protected:
    std::string textOf(const Message& message) {
        const auto& user = std::get<UserMessage>(message);
        return std::get<TextContent>(std::get<std::vector<UserContentBlock>>(user.content)[0]).text;
    }

    CustomMessage bash(Json data) {
        CustomMessage message;
        message.role = "bashExecution";
        message.data = std::move(data);
        message.timestamp = 5;
        return message;
    }

    AgentMessageConverter m_converter;
};

TEST_F(AgentMessageConverterTest, BashExecutionTextCoversOutcomes) {
    EXPECT_EQ(m_converter.bashExecutionText(Json{{"command", "ls"}, {"output", "a"}, {"exitCode", 0}}),
              "Ran `ls`\n```\na\n```");
    EXPECT_EQ(m_converter.bashExecutionText(Json{{"command", "x"}, {"output", ""}, {"exitCode", 2}}),
              "Ran `x`\n(no output)\n\nCommand exited with code 2");
    EXPECT_EQ(m_converter.bashExecutionText(Json{{"command", "x"}, {"cancelled", true}}),
              "Ran `x`\n(no output)\n\n(command cancelled)");
    EXPECT_EQ(m_converter.bashExecutionText(
                  Json{{"command", "x"}, {"output", "o"}, {"truncated", true}, {"fullOutputPath", "/t/f"}}),
              "Ran `x`\n```\no\n```\n\n[Output truncated. Full output: /t/f]");
}

TEST_F(AgentMessageConverterTest, ConvertDropsExcludedAndWrapsSummaries) {
    std::vector<AgentMessage> messages;
    messages.push_back(bash(Json{{"command", "ls"}, {"output", "a"}, {"excludeFromContext", true}}));
    messages.push_back(m_converter.compactionSummary("S", 10, "1970-01-01T00:00:01.000Z"));
    messages.push_back(m_converter.branchSummary("B", "id1", "1970-01-01T00:00:02.000Z"));
    messages.push_back(UserMessage{std::string("hi"), 3});
    const auto out = m_converter.convert(messages);
    ASSERT_EQ(out.size(), 3U);
    EXPECT_EQ(textOf(out[0]),
              "The conversation history before this point was compacted into the following summary:\n\n<summary>\nS\n</summary>");
    EXPECT_EQ(std::get<UserMessage>(out[0]).timestamp, 1000);
    EXPECT_EQ(textOf(out[1]),
              "The following is a summary of a branch that this conversation came back from:\n\n<summary>\nB</summary>");
    EXPECT_EQ(std::get<std::string>(std::get<UserMessage>(out[2]).content), "hi");
}

TEST_F(AgentMessageConverterTest, CustomMessageContentStringAndBlocks) {
    std::vector<AgentMessage> messages;
    messages.push_back(m_converter.custom("note", Json("plain"), true, Json(), "1970-01-01T00:00:00.000Z"));
    messages.push_back(m_converter.custom(
        "note", Json::array({Json{{"type", "text"}, {"text", "block"}}}), false, Json{{"k", 1}},
        "1970-01-01T00:00:00.000Z"));
    const auto out = m_converter.convert(messages);
    ASSERT_EQ(out.size(), 2U);
    EXPECT_EQ(std::get<std::string>(std::get<UserMessage>(out[0]).content), "plain");
    EXPECT_EQ(textOf(out[1]), "block");
    EXPECT_EQ(std::get<CustomMessage>(messages[1]).data["details"]["k"], 1);
}
