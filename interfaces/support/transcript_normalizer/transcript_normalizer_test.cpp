#include "interfaces/support/transcript_normalizer/transcript_normalizer.h"

#include <gtest/gtest.h>

class TranscriptNormalizerTest : public testing::Test {
protected:
    Tool tool(const std::string& name, const std::string& description = "d") {
        Tool t;
        t.name = name;
        t.description = description;
        t.parameters = Json::parse(R"({"type":"object"})");
        return t;
    }

    SystemMessage system(const std::string& text) {
        SystemMessage message;
        message.content = text;
        return message;
    }

    UserMessage user(const std::string& text) {
        UserMessage message;
        message.content = text;
        return message;
    }

    TranscriptNormalizer m_normalizer;
};

TEST_F(TranscriptNormalizerTest, EmptyContextStaysEmpty) {
    Context context;
    context.messages.emplace_back(user("hi"));
    const auto normalized = m_normalizer.normalizeContext(context);
    ASSERT_EQ(normalized.messages.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<UserMessage>(normalized.messages[0]));
}

TEST_F(TranscriptNormalizerTest, FoldsPromptAndToolsIntoLeadingMessage) {
    Context context;
    context.systemPrompt = "be nice";
    context.tools = std::vector<Tool>{tool("read")};
    context.messages.emplace_back(user("hi"));
    const auto normalized = m_normalizer.normalizeContext(context);
    ASSERT_EQ(normalized.messages.size(), 2U);
    const auto* head = std::get_if<SystemMessage>(&normalized.messages[0]);
    ASSERT_NE(head, nullptr);
    EXPECT_EQ(std::get<std::string>(head->content), "be nice");
    ASSERT_TRUE(head->toolsAdded.has_value());
    EXPECT_EQ(head->toolsAdded->size(), 1U);
}

TEST_F(TranscriptNormalizerTest, ReplaysToolAdditionsAndRemovals) {
    SystemMessage first = system("base");
    first.toolsAdded = std::vector<Tool>{tool("read"), tool("write")};
    SystemMessage second = system("");
    second.toolsRemoved = std::vector<ToolReference>{{"write"}};
    second.toolsAdded = std::vector<Tool>{tool("bash")};
    const std::vector<Message> messages{first, user("x"), second};
    const auto tools = m_normalizer.currentTools(messages);
    ASSERT_EQ(tools.size(), 2U);
    EXPECT_EQ(tools[0].name, "read");
    EXPECT_EQ(tools[1].name, "bash");
}

TEST_F(TranscriptNormalizerTest, ReplaysContentAndSections) {
    SystemMessage first = system("base");
    first.sections = std::vector<std::pair<std::string, std::optional<std::string>>>{
        {"a", "one"}, {"b", "two"}};
    SystemMessage second = system("more");
    second.sections = std::vector<std::pair<std::string, std::optional<std::string>>>{
        {"a", "uno"}, {"b", std::nullopt}};
    const std::vector<Message> messages{first, second};
    EXPECT_EQ(m_normalizer.currentSystemPrompt(messages), "base\n\nmore\n\nuno");
}

TEST_F(TranscriptNormalizerTest, CollapseKeepsHeadAndDropsLaterSystemMessages) {
    TranscriptContext context;
    context.messages = {system("base"), user("x"), system("later")};
    const auto collapsed = m_normalizer.collapseSystemMessages(context);
    ASSERT_EQ(collapsed.messages.size(), 2U);
    const auto* head = std::get_if<SystemMessage>(&collapsed.messages[0]);
    ASSERT_NE(head, nullptr);
    EXPECT_EQ(std::get<std::string>(head->content), "base\n\nlater");
    EXPECT_TRUE(std::holds_alternative<UserMessage>(collapsed.messages[1]));
}

TEST_F(TranscriptNormalizerTest, ToolStateChangesTreatRedefinitionAsRemoveAndAdd) {
    const auto changes = m_normalizer.toolStateChanges({tool("a"), tool("b")}, {tool("a"), tool("b", "new"), tool("c")});
    ASSERT_EQ(changes.toolsAdded.size(), 2U);
    EXPECT_EQ(changes.toolsAdded[0].name, "b");
    EXPECT_EQ(changes.toolsAdded[1].name, "c");
    ASSERT_EQ(changes.toolsRemoved.size(), 1U);
    EXPECT_EQ(changes.toolsRemoved[0].name, "b");
}

TEST_F(TranscriptNormalizerTest, RendersSectionUpdates) {
    SystemMessage message = system("note");
    message.sections = std::vector<std::pair<std::string, std::optional<std::string>>>{
        {"x", "val"}, {"y", std::nullopt}};
    EXPECT_EQ(m_normalizer.renderSystemMessageUpdate(message),
              "note\n\nUpdated system prompt section \"x\":\n\nval\n\nRemoved system prompt section \"y\".");
}
