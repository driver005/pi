#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.plugin_stream_translator;

class PluginStreamTranslatorTest : public testing::Test {
protected:
    PluginStreamTranslatorTest() {
        m_model.id = "m1";
        m_model.provider = "plug";
        m_model.api = "plug-api";
        m_model.cost.input = 1.0;
        m_model.cost.output = 2.0;
        m_signal = std::make_shared<AbortSignal>();
        m_translator = std::make_unique<PluginStreamTranslator>(m_model, m_signal, 42);
    }

    std::vector<AssistantMessageEvent> drain() {
        std::vector<AssistantMessageEvent> events;
        while (auto event = m_translator->stream()->next()) {
            events.push_back(*event);
        }
        return events;
    }

    Model m_model;
    std::shared_ptr<AbortSignal> m_signal;
    std::unique_ptr<PluginStreamTranslator> m_translator;
};

TEST_F(PluginStreamTranslatorTest, DeltasFormBlocksAndDoneEndsTheStream) {
    EXPECT_TRUE(m_translator->emit(R"({"type":"thinking_delta","delta":"hm"})"));
    EXPECT_TRUE(m_translator->emit(R"({"type":"thinking_delta","delta":"m"})"));
    EXPECT_TRUE(m_translator->emit(R"({"type":"text_delta","delta":"hel"})"));
    EXPECT_TRUE(m_translator->emit(R"({"type":"text_delta","delta":"lo"})"));
    EXPECT_TRUE(m_translator->emit(R"({"type":"usage","input":1000000,"output":500000})"));
    EXPECT_TRUE(m_translator->emit(R"({"type":"response","id":"r1","model":"m1-2025"})"));
    EXPECT_FALSE(m_translator->emit(R"({"type":"done"})"));
    const auto result = m_translator->stream()->result();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->content.size(), 2U);
    EXPECT_EQ(std::get<ThinkingContent>(result->content[0]).thinking, "hmm");
    EXPECT_EQ(std::get<TextContent>(result->content[1]).text, "hello");
    EXPECT_EQ(result->stopReason, StopReason::Stop);
    EXPECT_EQ(result->api, "plug-api");
    EXPECT_EQ(result->provider, "plug");
    EXPECT_EQ(result->model, "m1");
    EXPECT_EQ(result->timestamp, 42);
    EXPECT_EQ(result->responseId, "r1");
    EXPECT_EQ(result->responseModel, "m1-2025");
    EXPECT_EQ(result->usage.totalTokens, 1500000);
    EXPECT_DOUBLE_EQ(result->usage.cost.total, 2.0);
    const auto events = drain();
    EXPECT_EQ(events.front().type, AssistantEventType::Start);
    EXPECT_EQ(events.back().type, AssistantEventType::Done);
}

TEST_F(PluginStreamTranslatorTest, ToolCallsEndWithToolUse) {
    EXPECT_TRUE(m_translator->emit(R"({"type":"text_delta","delta":"let me"})"));
    EXPECT_TRUE(m_translator->emit(R"({"type":"tool_call","id":"c1","name":"read","arguments":{"path":"a.txt"}})"));
    EXPECT_FALSE(m_translator->emit(R"({"type":"done"})"));
    const auto result = m_translator->stream()->result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->stopReason, StopReason::ToolUse);
    ASSERT_EQ(result->content.size(), 2U);
    const ToolCall& call = std::get<ToolCall>(result->content[1]);
    EXPECT_EQ(call.id, "c1");
    EXPECT_EQ(call.name, "read");
    EXPECT_EQ(call.arguments["path"], "a.txt");
}

TEST_F(PluginStreamTranslatorTest, StopReasonLengthAndErrors) {
    EXPECT_FALSE(m_translator->emit(R"({"type":"done","stopReason":"length"})"));
    EXPECT_EQ(m_translator->stream()->result()->stopReason, StopReason::Length);

    PluginStreamTranslator failing(m_model, m_signal, 1);
    EXPECT_TRUE(failing.emit(R"({"type":"text_delta","delta":"par"})"));
    EXPECT_FALSE(failing.emit(R"({"type":"error","message":"boom"})"));
    const auto result = failing.stream()->result();
    EXPECT_EQ(result->stopReason, StopReason::Error);
    EXPECT_EQ(result->errorMessage, "boom");
    EXPECT_EQ(std::get<TextContent>(result->content[0]).text, "par");
}

TEST_F(PluginStreamTranslatorTest, InvalidEventsFinishTheStreamWithAnError) {
    for (const std::string event : {"not json", "[1]", R"({"type":5})", R"({"type":"nope"})", R"({"type":"text_delta"})", R"({"type":"tool_call","id":"x"})", R"({"type":"done","stopReason":"weird"})"}) {
        PluginStreamTranslator translator(m_model, m_signal, 1);
        EXPECT_FALSE(translator.emit(event)) << event;
        const auto result = translator.stream()->result();
        ASSERT_TRUE(result.has_value()) << event;
        EXPECT_EQ(result->stopReason, StopReason::Error) << event;
        EXPECT_FALSE(translator.emit(R"({"type":"text_delta","delta":"late"})"));
    }
}

TEST_F(PluginStreamTranslatorTest, AbortStopsTheStreamAndReportsAborted) {
    EXPECT_TRUE(m_translator->emit(R"({"type":"text_delta","delta":"a"})"));
    m_signal->abort();
    EXPECT_FALSE(m_translator->emit(R"({"type":"text_delta","delta":"b"})"));
    const auto result = m_translator->stream()->result();
    EXPECT_EQ(result->stopReason, StopReason::Aborted);
    EXPECT_EQ(std::get<TextContent>(result->content[0]).text, "a");
}

TEST_F(PluginStreamTranslatorTest, FinishClosesAStreamTheFunctionLeftOpen) {
    EXPECT_TRUE(m_translator->emit(R"({"type":"text_delta","delta":"a"})"));
    m_translator->finish();
    const auto result = m_translator->stream()->result();
    EXPECT_EQ(result->stopReason, StopReason::Error);
    EXPECT_NE(result->errorMessage->find("without a done or error"), std::string::npos);
    m_translator->finish();

    PluginStreamTranslator cancelled(m_model, m_signal, 1);
    m_signal->abort();
    cancelled.finish();
    EXPECT_EQ(cancelled.stream()->result()->stopReason, StopReason::Aborted);
}
