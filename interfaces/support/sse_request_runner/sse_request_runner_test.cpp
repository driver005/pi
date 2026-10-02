#include <gtest/gtest.h>

import std;
import pi.support.sse_request_runner;
import pi.testing.fixed_clock;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class SseRequestRunnerTest : public testing::Test {
protected:
    SseRequestRunnerTest() : m_emitter(initial()) {}

    AssistantMessage initial() {
        AssistantMessage message;
        message.stopReason = StopReason::Pending;
        return message;
    }

    HttpResponse reply(int status, std::string body = "") {
        HttpResponse response;
        response.status = status;
        response.body = std::move(body);
        return response;
    }

    void run(const SseRequestRunner::EventHandler& handler,
             const SseRequestRunner::Finisher& finish) {
        SseRequestRunner runner(m_sender);
        runner.run(
            HttpRequest{}, m_model, m_options, m_emitter, handler, finish,
            [](const HttpResponse& response) { return "http " + std::to_string(response.status); });
    }

    AssistantMessageEvent last() {
        auto stream = m_emitter.stream();
        std::optional<AssistantMessageEvent> event;
        std::optional<AssistantMessageEvent> previous;
        while ((event = stream->next())) {
            previous = event;
        }
        return *previous;
    }

    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    FixedClock m_clock;
    RetryingHttpSender m_sender{m_http, m_sleeper, m_clock};
    Model m_model;
    StreamOptions m_options;
    AssistantStreamEmitter m_emitter;
};

TEST_F(SseRequestRunnerTest, DeliversEventsInOrderThenFinishes) {
    m_http.enqueue(reply(200, "event: a\ndata: 1\n\nevent: b\ndata: 2\n\n"));
    std::vector<std::string> seen;
    run(
        [&](const SseEvent& event) -> Result<void> {
            seen.push_back(event.event + ":" + event.data);
            return {};
        },
        [&]() -> Result<void> {
            m_emitter.done(StopReason::Stop);
            return {};
        });
    EXPECT_EQ(seen, (std::vector<std::string>{"a:1", "b:2"}));
    const auto event = last();
    EXPECT_EQ(event.type, AssistantEventType::Done);
}

TEST_F(SseRequestRunnerTest, StartEmittedBeforeBody) {
    m_http.enqueue(reply(200, "data: x\n\n"));
    run([&](const SseEvent&) -> Result<void> { return {}; },
        [&]() -> Result<void> {
            m_emitter.done(StopReason::Stop);
            return {};
        });
    auto stream = m_emitter.stream();
    EXPECT_EQ(stream->next()->type, AssistantEventType::Start);
}

TEST_F(SseRequestRunnerTest, HandlerErrorEndsWithErrorEvent) {
    m_http.enqueue(reply(200, "data: x\n\n"));
    run([](const SseEvent&) -> Result<void> { return std::unexpected(Error{"x", "bad event"}); },
        []() -> Result<void> { return {}; });
    const auto event = last();
    EXPECT_EQ(event.type, AssistantEventType::Error);
    EXPECT_EQ(event.message->errorMessage, "bad event");
    EXPECT_EQ(event.message->stopReason, StopReason::Error);
}

TEST_F(SseRequestRunnerTest, HttpErrorUsesFormatter) {
    m_http.enqueue(reply(401, "nope"));
    run([](const SseEvent&) -> Result<void> { return {}; },
        []() -> Result<void> { return {}; });
    const auto event = last();
    EXPECT_EQ(event.message->errorMessage, "http 401");
}

TEST_F(SseRequestRunnerTest, TransportErrorReported) {
    m_http.enqueue(std::unexpected(Error{"transport", "connection refused"}));
    run([](const SseEvent&) -> Result<void> { return {}; },
        []() -> Result<void> { return {}; });
    EXPECT_EQ(last().message->errorMessage, "connection refused");
}

TEST_F(SseRequestRunnerTest, FinisherErrorReported) {
    m_http.enqueue(reply(200, ""));
    run([](const SseEvent&) -> Result<void> { return {}; },
        []() -> Result<void> { return std::unexpected(Error{"x", "ended early"}); });
    EXPECT_EQ(last().message->errorMessage, "ended early");
}

TEST_F(SseRequestRunnerTest, AbortedSignalYieldsAbortedStop) {
    m_options.signal = std::make_shared<AbortSignal>();
    m_options.signal->abort();
    m_http.enqueue(reply(200, "data: x\n\n"));
    run([](const SseEvent&) -> Result<void> { return {}; },
        []() -> Result<void> { return {}; });
    const auto event = last();
    EXPECT_EQ(event.message->stopReason, StopReason::Aborted);
    EXPECT_EQ(event.message->errorMessage, "Request was aborted");
}

TEST_F(SseRequestRunnerTest, RetriesBeforeStreaming) {
    m_options.maxRetries = 2;
    HttpResponse busy = reply(503);
    busy.headers = {{"retry-after-ms", "1"}};
    m_http.enqueue(busy);
    m_http.enqueue(reply(200, "data: ok\n\n"));
    std::vector<std::string> seen;
    run(
        [&](const SseEvent& event) -> Result<void> {
            seen.push_back(event.data);
            return {};
        },
        [&]() -> Result<void> {
            m_emitter.done(StopReason::Stop);
            return {};
        });
    EXPECT_EQ(seen, (std::vector<std::string>{"ok"}));
    EXPECT_EQ(m_http.calls(), 2);
}

TEST_F(SseRequestRunnerTest, OnResponseHookSeesStatus) {
    int status = 0;
    m_options.onResponse = [&](const ProviderResponse& response, const Model&) {
        status = response.status;
    };
    m_http.enqueue(reply(200, ""));
    run([](const SseEvent&) -> Result<void> { return {}; },
        [&]() -> Result<void> {
            m_emitter.done(StopReason::Stop);
            return {};
        });
    EXPECT_EQ(status, 200);
}
