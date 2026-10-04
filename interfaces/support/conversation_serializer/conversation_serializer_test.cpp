#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.conversation_serializer;

class ConversationSerializerTest : public testing::Test {
protected:
    ConversationSerializer m_serializer;
};

TEST_F(ConversationSerializerTest, SerializesRolesInOrder) {
    UserMessage user;
    user.content = std::string("hello");
    AssistantMessage assistant;
    assistant.content.push_back(ThinkingContent{"hmm", std::nullopt, std::nullopt});
    assistant.content.push_back(TextContent{"answer", std::nullopt});
    ToolCall call;
    call.name = "read";
    call.arguments = Json{{"path", "a.txt"}, {"limit", 5}};
    assistant.content.push_back(call);
    ToolResultMessage result;
    result.content.push_back(TextContent{"file body", std::nullopt});
    const std::string text = m_serializer.serialize({user, assistant, result});
    EXPECT_EQ(text,
              "[User]: hello\n\n[Assistant thinking]: hmm\n\n[Assistant]: answer\n\n"
              "[Assistant tool calls]: read(path=\"a.txt\", limit=5)\n\n[Tool result]: file body");
}

TEST_F(ConversationSerializerTest, SkipsEmptyMessagesAndTruncatesToolResults) {
    UserMessage empty;
    empty.content = std::string();
    ToolResultMessage result;
    result.content.push_back(TextContent{std::string(2005, 'x'), std::nullopt});
    const std::string text = m_serializer.serialize({empty, result});
    EXPECT_EQ(text, "[Tool result]: " + std::string(2000, 'x') + "\n\n[... 5 more characters truncated]");
}

TEST_F(ConversationSerializerTest, TruncationKeepsUtf8Intact) {
    ToolResultMessage result;
    std::string body;
    for (int i = 0; i < 2001; ++i) {
        body += "\xC3\xA9";
    }
    result.content.push_back(TextContent{body, std::nullopt});
    const std::string text = m_serializer.serialize({result});
    EXPECT_NE(text.find("[... 1 more characters truncated]"), std::string::npos);
    EXPECT_EQ(text.substr(15, 2), "\xC3\xA9");
}
