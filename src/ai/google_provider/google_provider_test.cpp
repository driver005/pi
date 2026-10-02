#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.google_provider;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class GoogleProviderTest : public testing::Test {
protected:
    Model model() {
        Model m;
        m.id = "gemini-2.5-flash";
        m.api = "google-generative-ai";
        m.provider = "google";
        m.baseUrl = "https://generativelanguage.googleapis.com/v1beta/";
        m.maxTokens = 2000;
        m.contextWindow = 100000;
        return m;
    }

    TranscriptContext context(const std::string& prompt) {
        UserMessage user;
        user.content = prompt;
        TranscriptContext c;
        c.messages.emplace_back(user);
        return c;
    }

    HttpResponse reply() {
        HttpResponse response;
        response.status = 200;
        response.body =
            R"(data: {"responseId":"r1","candidates":[{"content":{"parts":[{"text":"Hi"}]},"finishReason":"STOP"}],)"
            R"("usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":3,"totalTokenCount":13}})"
            "\n\n";
        return response;
    }

    AssistantMessage result(const std::shared_ptr<AssistantMessageStream>& stream) {
        while (stream->next()) {
        }
        return *stream->result();
    }

    StreamOptions keyed() {
        StreamOptions options;
        options.apiKey = "g-key";
        return options;
    }

    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    FixedClock m_clock;
    InlineExecutor m_executor;
    GoogleProvider m_provider{m_http, m_sleeper, m_clock, m_executor};
    HeaderMerger m_headers;
};

TEST_F(GoogleProviderTest, StreamsTextResponse) {
    m_http.enqueue(reply());
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), keyed()));
    ASSERT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(message.api, "google-generative-ai");
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "Hi");
    EXPECT_EQ(message.usage.input, 10);
    EXPECT_EQ(message.usage.output, 3);
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url,
              "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:streamGenerateContent?alt=sse");
    EXPECT_EQ(m_headers.find(request.headers, "x-goog-api-key"), "g-key");
    const Json body = Json::parse(request.body);
    EXPECT_EQ(body["contents"][0]["parts"][0]["text"], "hello");
}

TEST_F(GoogleProviderTest, DefaultBaseUrlAndQualifiedModelIds) {
    Model m = model();
    m.baseUrl = "";
    m.id = "tunedModels/mine";
    m_http.enqueue(reply());
    result(m_provider.stream(m, context("hello"), keyed()));
    EXPECT_EQ(m_http.requests()[0].url,
              "https://generativelanguage.googleapis.com/v1beta/tunedModels/mine:streamGenerateContent?alt=sse");
}

TEST_F(GoogleProviderTest, MissingKeyFailsWithoutRequest) {
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), {}));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, "No API key for provider: google");
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(GoogleProviderTest, HttpErrorShowsTheBody) {
    HttpResponse failure;
    failure.status = 400;
    failure.body = R"({ "error": { "code": 400, "message": "API key not valid", "status": "INVALID_ARGUMENT" } })";
    m_http.enqueue(failure);
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), keyed()));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage,
              R"({"error":{"code":400,"message":"API key not valid","status":"INVALID_ARGUMENT"}})");
}

TEST_F(GoogleProviderTest, UnusableThinkingMappingFailsBeforeTheRequest) {
    Model m = model();
    m.id = "gemini-3-pro";
    m.reasoning = true;
    m.thinkingLevelMap = Json::parse(R"({"high":"extreme"})");
    StreamOptions options = keyed();
    options.reasoning = ThinkingLevel::High;
    const AssistantMessage message = result(m_provider.stream(m, context("hello"), options));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(GoogleProviderTest, SafetyBlockIsAnError) {
    HttpResponse blocked;
    blocked.status = 200;
    blocked.body = R"(data: {"candidates":[{"finishReason":"SAFETY"}]})"
                   "\n\n";
    m_http.enqueue(blocked);
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), keyed()));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, "Provider stopped with: SAFETY");
}
