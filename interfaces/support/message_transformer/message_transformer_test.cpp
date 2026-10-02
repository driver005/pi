#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.message_transformer;

class MessageTransformerTest : public testing::Test {
protected:
    Model model(const std::string& id = "m1") {
        Model m;
        m.id = id;
        m.api = "anthropic-messages";
        m.provider = "anthropic";
        return m;
    }

    AssistantMessage assistant(const std::string& id = "m1") {
        AssistantMessage a;
        a.api = "anthropic-messages";
        a.provider = "anthropic";
        a.model = id;
        return a;
    }

    UserMessage user(const std::string& text) {
        UserMessage u;
        u.content = text;
        return u;
    }

    ToolCall call(const std::string& id) {
        ToolCall c;
        c.id = id;
        c.name = "read";
        return c;
    }

    MessageTransformer m_transformer;
};

TEST_F(MessageTransformerTest, SynthesizesResultForOrphanedToolCall) {
    AssistantMessage a = assistant();
    a.content.emplace_back(call("t1"));
    std::vector<Message> in = {user("go"), a, user("next")};
    const auto out = m_transformer.transform(in, model(), nullptr, 42);
    ASSERT_EQ(out.size(), 4U);
    const auto* result = std::get_if<ToolResultMessage>(&out[2]);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->toolCallId, "t1");
    EXPECT_TRUE(result->isError);
    EXPECT_EQ(result->timestamp, 42);
}

TEST_F(MessageTransformerTest, KeepsAnsweredToolCalls) {
    AssistantMessage a = assistant();
    a.content.emplace_back(call("t1"));
    ToolResultMessage r;
    r.toolCallId = "t1";
    std::vector<Message> in = {user("go"), a, r};
    EXPECT_EQ(m_transformer.transform(in, model(), nullptr, 0).size(), 3U);
}

TEST_F(MessageTransformerTest, DropsErroredAssistant) {
    AssistantMessage a = assistant();
    a.stopReason = StopReason::Error;
    std::vector<Message> in = {user("go"), a};
    EXPECT_EQ(m_transformer.transform(in, model(), nullptr, 0).size(), 1U);
}

TEST_F(MessageTransformerTest, FlattensThinkingFromOtherModel) {
    AssistantMessage a = assistant("other");
    ThinkingContent thinking;
    thinking.thinking = "hmm";
    thinking.thinkingSignature = "sig";
    a.content.emplace_back(thinking);
    ToolCall c = call("a|b");
    c.thoughtSignature = "x";
    a.content.emplace_back(c);
    ToolResultMessage r;
    r.toolCallId = "a|b";
    std::vector<Message> in = {a, r};
    const auto out = m_transformer.transform(
        in, model(),
        [this](const std::string& id, const Model&, const AssistantMessage&) {
            return m_transformer.sanitizeToolCallId(id, 64);
        },
        0);
    const auto& converted = std::get<AssistantMessage>(out[0]);
    EXPECT_EQ(std::get<TextContent>(converted.content[0]).text, "hmm");
    const auto& toolCall = std::get<ToolCall>(converted.content[1]);
    EXPECT_EQ(toolCall.id, "a_b");
    EXPECT_FALSE(toolCall.thoughtSignature.has_value());
    EXPECT_EQ(std::get<ToolResultMessage>(out[1]).toolCallId, "a_b");
}

TEST_F(MessageTransformerTest, KeepsSignedThinkingForSameModel) {
    AssistantMessage a = assistant();
    ThinkingContent thinking;
    thinking.thinking = "";
    thinking.thinkingSignature = "sig";
    a.content.emplace_back(thinking);
    std::vector<Message> in = {a};
    const auto out = m_transformer.transform(in, model(), nullptr, 0);
    EXPECT_TRUE(std::holds_alternative<ThinkingContent>(
        std::get<AssistantMessage>(out[0]).content[0]));
}

TEST_F(MessageTransformerTest, ReplacesImagesForTextOnlyModel) {
    Model m = model();
    m.input = {"text"};
    UserMessage u;
    u.content = std::vector<UserContentBlock>{ImageContent{"d", "image/png"},
                                              ImageContent{"d", "image/png"},
                                              TextContent{"hi", std::nullopt}};
    std::vector<Message> in = {u};
    const auto out = m_transformer.transform(in, m, nullptr, 0);
    const auto& blocks =
        std::get<std::vector<UserContentBlock>>(std::get<UserMessage>(out[0]).content);
    ASSERT_EQ(blocks.size(), 2U);
    EXPECT_EQ(std::get<TextContent>(blocks[0]).text,
              "(image omitted: model does not support images)");
}

TEST_F(MessageTransformerTest, HoldsSystemMessageUntilToolResults) {
    AssistantMessage a = assistant();
    a.content.emplace_back(call("t1"));
    SystemMessage s;
    s.content = "note";
    ToolResultMessage r;
    r.toolCallId = "t1";
    std::vector<Message> in = {a, s, r};
    const auto out = m_transformer.transform(in, model(), nullptr, 0);
    ASSERT_EQ(out.size(), 3U);
    EXPECT_TRUE(std::holds_alternative<ToolResultMessage>(out[1]));
    EXPECT_TRUE(std::holds_alternative<SystemMessage>(out[2]));
}
