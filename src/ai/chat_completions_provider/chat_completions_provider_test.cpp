#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.chat_completions_provider;
import pi.base.curl_http_client;
import pi.base.thread_pool;
import pi.testing.fake_http_server;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class ChatCompletionsProviderTest : public testing::Test {
protected:
    Model model() {
        Model m;
        m.id = "gpt-test";
        m.api = "openai-completions";
        m.provider = "openai";
        m.baseUrl = "https://api.openai.com/v1/";
        m.maxTokens = 2000;
        m.contextWindow = 100000;
        m.cost.input = 1;
        m.cost.output = 2;
        return m;
    }

    TranscriptContext context(const std::string& prompt) {
        UserMessage user;
        user.content = prompt;
        TranscriptContext c;
        c.messages.emplace_back(user);
        return c;
    }

    HttpResponse sse(const std::string& body) {
        HttpResponse response;
        response.status = 200;
        response.body = body;
        return response;
    }

    std::string helloStream() {
        return R"(data: {"id":"c1","model":"gpt-test","choices":[{"delta":{"content":"Hi"}}]})"
               "\n\n"
               R"(data: {"id":"c1","choices":[{"delta":{},"finish_reason":"stop"}]})"
               "\n\n"
               R"(data: {"id":"c1","choices":[],"usage":{"prompt_tokens":10,"completion_tokens":3}})"
               "\n\n"
               "data: [DONE]\n\n";
    }

    AssistantMessage result(const std::shared_ptr<AssistantMessageStream>& stream) {
        while (stream->next()) {
        }
        return *stream->result();
    }

    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    FixedClock m_clock;
    InlineExecutor m_executor;
    ChatCompletionsProvider m_provider{m_http, m_sleeper, m_clock, m_executor};
    HeaderMerger m_headers;
};

TEST_F(ChatCompletionsProviderTest, StreamsTextResponse) {
    m_http.enqueue(sse(helloStream()));
    StreamOptions options;
    options.apiKey = "sk-test";
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    ASSERT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "Hi");
    EXPECT_EQ(message.usage.input, 10);
    EXPECT_EQ(message.usage.output, 3);
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://api.openai.com/v1/chat/completions");
    EXPECT_EQ(m_headers.find(request.headers, "authorization"), "Bearer sk-test");
    EXPECT_EQ(m_headers.find(request.headers, "content-type"), "application/json");
    const Json body = Json::parse(request.body);
    EXPECT_EQ(body["model"], "gpt-test");
    EXPECT_EQ(body["messages"][0]["content"], "hello");
}

TEST_F(ChatCompletionsProviderTest, MissingKeyFailsWithoutRequest) {
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), {}));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, "No API key for provider: openai");
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(ChatCompletionsProviderTest, HeaderOwnedAuthKeepsCallerAuthorization) {
    m_http.enqueue(sse(helloStream()));
    StreamOptions options;
    options.headers = {{"Authorization", "Bearer from-header"}};
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(m_headers.find(m_http.requests()[0].headers, "authorization"), "Bearer from-header");
}

TEST_F(ChatCompletionsProviderTest, HttpErrorReportedOpenAiStyle) {
    HttpResponse failure;
    failure.status = 401;
    failure.body = R"({"error":{"message":"Incorrect API key","type":"invalid_request_error"}})";
    m_http.enqueue(failure);
    StreamOptions options;
    options.apiKey = "bad";
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, "401 Incorrect API key");
}

TEST_F(ChatCompletionsProviderTest, RetriesRateLimit) {
    HttpResponse limited;
    limited.status = 429;
    limited.headers = {{"retry-after-ms", "3"}};
    m_http.enqueue(limited);
    m_http.enqueue(sse(helloStream()));
    StreamOptions options;
    options.apiKey = "k";
    options.maxRetries = 1;
    const AssistantMessage message = result(m_provider.stream(model(), context("hi"), options));
    EXPECT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(m_sleeper.delays(), (std::vector<std::int64_t>{3}));
}

TEST_F(ChatCompletionsProviderTest, OpenRouterSessionAffinityHeader) {
    m_http.enqueue(sse(helloStream()));
    Model m = model();
    m.provider = "openrouter";
    m.baseUrl = "https://openrouter.ai/api/v1";
    StreamOptions options;
    options.apiKey = "k";
    options.sessionId = "sess";
    result(m_provider.stream(m, context("hi"), options));
    EXPECT_EQ(m_headers.find(m_http.requests()[0].headers, "x-session-id"), "sess");
}

TEST_F(ChatCompletionsProviderTest, CopilotGetsDynamicHeaders) {
    m_http.enqueue(sse(helloStream()));
    Model m = model();
    m.provider = "github-copilot";
    m.baseUrl = "https://api.githubcopilot.com";
    StreamOptions options;
    options.apiKey = "k";
    result(m_provider.stream(m, context("hi"), options));
    EXPECT_EQ(m_headers.find(m_http.requests()[0].headers, "x-initiator"), "user");
    EXPECT_EQ(m_headers.find(m_http.requests()[0].headers, "openai-intent"), "conversation-edits");
}

TEST(ChatCompletionsProviderEndToEndTest, StreamsOverRealHttp) {
    const std::string events =
        R"(data: {"id":"c","choices":[{"delta":{"content":"wire"}}]})"
        "\n\n"
        R"(data: {"id":"c","choices":[{"delta":{},"finish_reason":"stop"}]})"
        "\n\n"
        "data: [DONE]\n\n";
    FakeHttpServer server([&](const FakeHttpRequest&) {
        FakeHttpReply reply;
        reply.headers = {{"Content-Type", "text/event-stream"}};
        reply.chunks = {events.substr(0, 20), events.substr(20)};
        return reply;
    });
    CurlHttpClient http;
    RecordingSleeper sleeper;
    FixedClock clock;
    ThreadPool pool(1);
    ChatCompletionsProvider provider(http, sleeper, clock, pool);
    Model model;
    model.id = "m";
    model.api = "openai-completions";
    model.provider = "openai";
    model.baseUrl = "http://127.0.0.1:" + std::to_string(server.port()) + "/v1";
    model.maxTokens = 100;
    UserMessage user;
    user.content = std::string("hi");
    TranscriptContext context;
    context.messages.emplace_back(user);
    StreamOptions options;
    options.apiKey = "k";
    auto stream = provider.stream(model, context, options);
    while (stream->next()) {
    }
    const AssistantMessage message = *stream->result();
    ASSERT_EQ(message.stopReason, StopReason::Stop) << message.errorMessage.value_or("");
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "wire");
    EXPECT_EQ(server.requests()[0].path, "/v1/chat/completions");
}
