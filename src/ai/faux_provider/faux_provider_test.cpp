#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.faux_provider;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;

class FauxProviderTest : public testing::Test {
protected:
    std::vector<AssistantMessageEvent> drain(const std::shared_ptr<AssistantMessageStream>& stream) {
        std::vector<AssistantMessageEvent> events;
        while (auto event = stream->next()) {
            events.push_back(*event);
        }
        return events;
    }

    InlineExecutor m_executor;
    FixedClock m_clock;
    FauxProvider m_provider{m_executor, m_clock};
    Model m_model = [] {
        Model model;
        model.id = "faux-1";
        model.api = "faux";
        model.provider = "faux";
        return model;
    }();
};

TEST_F(FauxProviderTest, StreamsTextResponseInChunks) {
    m_provider.enqueue(m_provider.textResponse("hello world"));
    const auto stream = m_provider.stream(m_model, TranscriptContext{}, StreamOptions{});
    const auto events = drain(stream);
    EXPECT_EQ(events.front().type, AssistantEventType::Start);
    EXPECT_EQ(events.back().type, AssistantEventType::Done);
    int deltas = 0;
    for (const auto& event : events) {
        deltas += event.type == AssistantEventType::TextDelta ? 1 : 0;
    }
    EXPECT_EQ(deltas, 3);
    const auto result = stream->result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<TextContent>(result->content[0]).text, "hello world");
    EXPECT_EQ(result->model, "faux-1");
    EXPECT_EQ(result->timestamp, m_clock.nowMs());
    EXPECT_GT(result->usage.output, 0);
}

TEST_F(FauxProviderTest, ToolCallResponseStopsWithToolUse) {
    m_provider.enqueue(m_provider.toolCallResponse("read", Json::parse(R"({"path":"a"})"), "c1"));
    const auto stream = m_provider.stream(m_model, TranscriptContext{}, StreamOptions{});
    drain(stream);
    const auto result = stream->result();
    EXPECT_EQ(result->stopReason, StopReason::ToolUse);
    EXPECT_EQ(std::get<ToolCall>(result->content[0]).arguments["path"], "a");
}

TEST_F(FauxProviderTest, EmptyQueueIsAnErrorResponse) {
    const auto stream = m_provider.stream(m_model, TranscriptContext{}, StreamOptions{});
    const auto events = drain(stream);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events.back().type, AssistantEventType::Error);
    EXPECT_EQ(stream->result()->errorMessage, "No more faux responses queued");
}

TEST_F(FauxProviderTest, FactoryReceivesContextAndCountsCalls) {
    int seen = -1;
    m_provider.enqueue([&](const TranscriptContext& context, const StreamOptions&, const Model&) {
        seen = static_cast<int>(context.messages.size());
        return m_provider.textResponse("ok");
    });
    TranscriptContext context;
    context.messages.emplace_back(UserMessage{});
    drain(m_provider.stream(m_model, context, StreamOptions{}));
    EXPECT_EQ(seen, 1);
    EXPECT_EQ(m_provider.callCount(), 1);
}

TEST_F(FauxProviderTest, AbortBeforeStreamingProducesAbortedError) {
    m_provider.enqueue(m_provider.textResponse("some long text to stream"));
    StreamOptions options;
    options.signal = std::make_shared<AbortSignal>();
    options.signal->abort();
    const auto stream = m_provider.stream(m_model, TranscriptContext{}, options);
    drain(stream);
    EXPECT_EQ(stream->result()->stopReason, StopReason::Aborted);
}

TEST_F(FauxProviderTest, ScriptedErrorResponsePropagates) {
    AssistantMessage failing;
    failing.stopReason = StopReason::Error;
    failing.errorMessage = "overloaded";
    m_provider.enqueue(failing);
    const auto stream = m_provider.stream(m_model, TranscriptContext{}, StreamOptions{});
    drain(stream);
    EXPECT_EQ(stream->result()->errorMessage, "overloaded");
}

TEST_F(FauxProviderTest, DeferredRequestsAnswerWithAHandleAndResolveAfterThePendingFetches) {
    m_provider.setDeferredBehavior(2, 25);
    m_provider.enqueue(m_provider.textResponse("final"));
    StreamOptions options;
    options.deferred = true;
    auto first = m_provider.stream(m_model, TranscriptContext{}, options);
    drain(first);
    ASSERT_EQ(first->result()->stopReason, StopReason::Deferred);
    ASSERT_TRUE(first->result()->deferred.has_value());
    const DeferredHandle handle = *first->result()->deferred;
    EXPECT_EQ(handle.provider, "faux");
    EXPECT_EQ(handle.modelId, "faux-1");
    EXPECT_EQ(handle.pollAfterMs, 25);
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto pending = m_provider.fetchDeferred(m_model, handle, StreamOptions{});
        drain(pending);
        EXPECT_EQ(pending->result()->stopReason, StopReason::Deferred);
        EXPECT_EQ(pending->result()->deferred->id, handle.id);
    }
    auto final = m_provider.fetchDeferred(m_model, handle, StreamOptions{});
    drain(final);
    EXPECT_EQ(final->result()->stopReason, StopReason::Stop);
    EXPECT_EQ(std::get<TextContent>(final->result()->content[0]).text, "final");
    EXPECT_EQ(m_provider.deferredFetchCount(), 3);
}

TEST_F(FauxProviderTest, CancelledAndUnknownHandlesFailTheirFetch) {
    m_provider.enqueue(m_provider.textResponse("final"));
    StreamOptions options;
    options.deferred = true;
    auto first = m_provider.stream(m_model, TranscriptContext{}, options);
    drain(first);
    const DeferredHandle handle = *first->result()->deferred;
    ASSERT_TRUE(m_provider.cancelDeferred(m_model, handle, StreamOptions{}).has_value());
    ASSERT_EQ(m_provider.cancelledDeferred().size(), 1u);
    auto cancelled = m_provider.fetchDeferred(m_model, handle, StreamOptions{});
    drain(cancelled);
    EXPECT_EQ(cancelled->result()->stopReason, StopReason::Error);
    EXPECT_NE(cancelled->result()->errorMessage->find("was cancelled"), std::string::npos);
    DeferredHandle unknown = handle;
    unknown.id = "nope";
    auto missing = m_provider.fetchDeferred(m_model, unknown, StreamOptions{});
    drain(missing);
    EXPECT_NE(missing->result()->errorMessage->find("Unknown faux deferred response"), std::string::npos);
}

TEST_F(FauxProviderTest, WithoutTheDeferredOptionResponsesStreamDirectly) {
    m_provider.setDeferredBehavior(5, 25);
    m_provider.enqueue(m_provider.textResponse("now"));
    auto stream = m_provider.stream(m_model, TranscriptContext{}, StreamOptions{});
    drain(stream);
    EXPECT_EQ(stream->result()->stopReason, StopReason::Stop);
    EXPECT_FALSE(stream->result()->deferred.has_value());
}
