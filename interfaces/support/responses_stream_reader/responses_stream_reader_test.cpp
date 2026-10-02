#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.responses_stream_reader;

class ResponsesStreamReaderTest : public testing::Test {
protected:
    ResponsesStreamReaderTest() : m_emitter(initial()) {
        m_model.id = "gpt";
        m_model.cost.input = 1;
        m_model.cost.output = 2;
        m_model.cost.cacheRead = 0.5;
        m_reader = std::make_unique<ResponsesStreamReader>(m_emitter, m_model);
        m_emitter.start();
    }

    AssistantMessage initial() {
        AssistantMessage message;
        message.stopReason = StopReason::Pending;
        return message;
    }

    Result<void> event(const std::string& json) {
        SseEvent sse;
        sse.data = json;
        return m_reader->handle(sse);
    }

    void textItem(const std::string& text, int index = 0) {
        const std::string i = std::to_string(index);
        ASSERT_TRUE(event(R"({"type":"response.output_item.added","output_index":)" + i +
                          R"(,"item":{"type":"message","id":"msg_1","role":"assistant","content":[]}})"));
        ASSERT_TRUE(event(R"({"type":"response.output_text.delta","output_index":)" + i + R"(,"delta":")" +
                          text + R"("})"));
        ASSERT_TRUE(event(R"({"type":"response.output_item.done","output_index":)" + i +
                          R"(,"item":{"type":"message","id":"msg_1","phase":"final_answer","content":[{"type":"output_text","text":")" +
                          text + R"("}]}})"));
    }

    Result<void> complete(const std::string& response = R"({"id":"resp_1","status":"completed"})") {
        return event(R"({"type":"response.completed","response":)" + response + "}");
    }

    std::vector<AssistantMessageEvent> events() {
        std::vector<AssistantMessageEvent> out;
        auto stream = m_emitter.stream();
        while (auto next = stream->next()) {
            out.push_back(*next);
        }
        return out;
    }

    Model m_model;
    AssistantStreamEmitter m_emitter;
    std::unique_ptr<ResponsesStreamReader> m_reader;
};

TEST_F(ResponsesStreamReaderTest, StreamsTextWithSignatureAndUsage) {
    ASSERT_TRUE(event(R"({"type":"response.created","response":{"id":"resp_1"}})"));
    textItem("Hello");
    ASSERT_TRUE(complete(R"({"id":"resp_1","status":"completed","usage":{"input_tokens":1000000,
        "output_tokens":500000,"total_tokens":1500000,"input_tokens_details":{"cached_tokens":400000},
        "output_tokens_details":{"reasoning_tokens":7}}})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& message = m_emitter.message();
    EXPECT_EQ(message.responseId, "resp_1");
    const auto& text = std::get<TextContent>(message.content[0]);
    EXPECT_EQ(text.text, "Hello");
    EXPECT_EQ(Json::parse(*text.textSignature), (Json{{"v", 1}, {"id", "msg_1"}, {"phase", "final_answer"}}));
    EXPECT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(message.usage.input, 600000);
    EXPECT_EQ(message.usage.cacheRead, 400000);
    EXPECT_EQ(message.usage.output, 500000);
    EXPECT_EQ(message.usage.reasoning, 7);
    EXPECT_EQ(message.usage.totalTokens, 1500000);
    EXPECT_DOUBLE_EQ(message.usage.cost.total, 0.6 + 1.0 + 0.2);
    EXPECT_EQ(message.rawStopReason, "completed");
    const auto all = events();
    EXPECT_EQ(all.front().type, AssistantEventType::Start);
    EXPECT_EQ(all.back().type, AssistantEventType::Done);
}

TEST_F(ResponsesStreamReaderTest, ReasoningKeepsTheItemAsSignature) {
    ASSERT_TRUE(event(R"({"type":"response.output_item.added","output_index":0,"item":{"type":"reasoning","id":"rs_1"}})"));
    ASSERT_TRUE(event(R"({"type":"response.reasoning_summary_text.delta","output_index":0,"delta":"think"})"));
    ASSERT_TRUE(event(R"({"type":"response.reasoning_summary_part.done","output_index":0})"));
    ASSERT_TRUE(event(R"({"type":"response.output_item.done","output_index":0,"item":{"type":"reasoning","id":"rs_1",
        "summary":[{"text":"first"},{"text":"second"}],"encrypted_content":null}})"));
    textItem("done", 1);
    ASSERT_TRUE(complete());
    ASSERT_TRUE(m_reader->finish());
    const auto& thinking = std::get<ThinkingContent>(m_emitter.message().content[0]);
    EXPECT_EQ(thinking.thinking, "first\n\nsecond");
    const Json signature = Json::parse(*thinking.thinkingSignature);
    EXPECT_EQ(signature["id"], "rs_1");
}

TEST_F(ResponsesStreamReaderTest, TerminalResponseBackfillsEncryptedReasoning) {
    ASSERT_TRUE(event(R"({"type":"response.output_item.added","output_index":0,"item":{"type":"reasoning","id":"rs_1"}})"));
    ASSERT_TRUE(event(R"({"type":"response.output_item.done","output_index":0,"item":{"type":"reasoning","id":"rs_1","summary":[]}})"));
    ASSERT_TRUE(complete(R"({"status":"completed","output":[{"type":"reasoning","id":"rs_1","encrypted_content":"enc"}]})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& thinking = std::get<ThinkingContent>(m_emitter.message().content[0]);
    EXPECT_EQ(Json::parse(*thinking.thinkingSignature)["encrypted_content"], "enc");
}

TEST_F(ResponsesStreamReaderTest, FunctionCallsBecomeToolUse) {
    ASSERT_TRUE(event(R"({"type":"response.output_item.added","output_index":0,
        "item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"read","arguments":""}})"));
    ASSERT_TRUE(event(R"({"type":"response.function_call_arguments.delta","output_index":0,"delta":"{\"path\":"})"));
    ASSERT_TRUE(event(R"({"type":"response.function_call_arguments.delta","output_index":0,"delta":"\"a\"}"})"));
    ASSERT_TRUE(event(R"({"type":"response.function_call_arguments.done","output_index":0,"arguments":"{\"path\":\"a\"}"})"));
    ASSERT_TRUE(event(R"({"type":"response.output_item.done","output_index":0,
        "item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"read","arguments":"{\"path\":\"a\"}","namespace":"ns"}})"));
    ASSERT_TRUE(complete());
    ASSERT_TRUE(m_reader->finish());
    const auto& message = m_emitter.message();
    const auto& call = std::get<ToolCall>(message.content[0]);
    EXPECT_EQ(call.id, "call_1|fc_1");
    EXPECT_EQ(call.name, "read");
    EXPECT_EQ(call.arguments, (Json{{"path", "a"}}));
    EXPECT_EQ(call.toolNamespace, "ns");
    EXPECT_EQ(message.stopReason, StopReason::ToolUse);
}

TEST_F(ResponsesStreamReaderTest, ArgumentsOnlyInTheDoneItemAreStillUsed) {
    ASSERT_TRUE(event(R"({"type":"response.output_item.added","output_index":0,
        "item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"read"}})"));
    ASSERT_TRUE(event(R"({"type":"response.output_item.done","output_index":0,
        "item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"read","arguments":"{\"a\":1}"}})"));
    ASSERT_TRUE(complete());
    ASSERT_TRUE(m_reader->finish());
    EXPECT_EQ(std::get<ToolCall>(m_emitter.message().content[0]).arguments, (Json{{"a", 1}}));
}

TEST_F(ResponsesStreamReaderTest, UnfinishedToolCallIsRejected) {
    ASSERT_TRUE(event(R"({"type":"response.output_item.added","output_index":0,
        "item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"read"}})"));
    ASSERT_TRUE(complete());
    const auto result = m_reader->finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("unfinished tool call: read"), std::string::npos);
}

TEST_F(ResponsesStreamReaderTest, RefusalsAreTextAndIncompleteMapsToLength) {
    ASSERT_TRUE(event(R"({"type":"response.output_item.added","output_index":0,"item":{"type":"message","id":"msg_1"}})"));
    ASSERT_TRUE(event(R"({"type":"response.refusal.delta","output_index":0,"delta":"no"})"));
    ASSERT_TRUE(event(R"({"type":"response.output_item.done","output_index":0,
        "item":{"type":"message","id":"msg_1","content":[{"type":"refusal","refusal":"no way"}]}})"));
    ASSERT_TRUE(event(R"({"type":"response.incomplete","response":{"status":"incomplete",
        "incomplete_details":{"reason":"max_output_tokens"}}})"));
    ASSERT_TRUE(m_reader->finish());
    EXPECT_EQ(std::get<TextContent>(m_emitter.message().content[0]).text, "no way");
    EXPECT_EQ(m_emitter.message().stopReason, StopReason::Length);
    EXPECT_EQ(m_emitter.message().rawStopReason, "incomplete.max_output_tokens");
}

TEST_F(ResponsesStreamReaderTest, OtherIncompleteReasonsAreErrors) {
    ASSERT_TRUE(event(R"({"type":"response.incomplete","response":{"status":"incomplete",
        "incomplete_details":{"reason":"content_filter"}}})"));
    const auto result = m_reader->finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Response incomplete: content_filter");
}

TEST_F(ResponsesStreamReaderTest, FailedResponseAndErrorEvents) {
    const auto failed = event(R"({"type":"response.failed","response":{"status":"failed",
        "error":{"code":"server_error","message":"boom"}}})");
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "server_error: boom");
    const auto error = event(R"({"type":"error","code":"rate_limit","message":"slow down"})");
    ASSERT_FALSE(error.has_value());
    EXPECT_EQ(error.error().message, "Error Code rate_limit: slow down");
}

TEST_F(ResponsesStreamReaderTest, StreamWithoutTerminalEventIsAnError) {
    textItem("partial");
    const auto result = m_reader->finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("before a terminal response event"), std::string::npos);
}

TEST_F(ResponsesStreamReaderTest, BadJsonAndUnknownEventsAreHandled) {
    EXPECT_FALSE(event("{not json").has_value());
    EXPECT_TRUE(event(R"({"type":"response.in_progress"})").has_value());
    EXPECT_TRUE(event("[DONE]").has_value());
}
