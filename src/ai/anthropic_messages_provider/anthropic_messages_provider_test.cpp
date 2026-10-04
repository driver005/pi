#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.anthropic_messages_provider;
import pi.base.curl_http_client;
import pi.base.thread_pool;
import pi.testing.fake_http_server;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class AnthropicMessagesProviderTest : public testing::Test {
protected:
    Model model() {
        Model m;
        m.id = "claude-sonnet-4";
        m.name = "Claude";
        m.api = "anthropic-messages";
        m.provider = "anthropic";
        m.baseUrl = "https://api.anthropic.com/";
        m.maxTokens = 8000;
        m.contextWindow = 200000;
        m.cost.input = 3;
        m.cost.output = 15;
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
        response.headers = {{"content-type", "text/event-stream"}};
        response.body = body;
        return response;
    }

    std::string helloStream() {
        return "event: message_start\ndata: "
               R"({"type":"message_start","message":{"id":"msg_1","model":"claude-sonnet-4","usage":{"input_tokens":10,"output_tokens":1}}})"
               "\n\n"
               "event: content_block_start\ndata: "
               R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"
               "\n\n"
               "event: content_block_delta\ndata: "
               R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hi"}})"
               "\n\n"
               "event: content_block_stop\ndata: "
               R"({"type":"content_block_stop","index":0})"
               "\n\n"
               "event: message_delta\ndata: "
               R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":5}})"
               "\n\n"
               "event: message_stop\ndata: "
               R"({"type":"message_stop"})"
               "\n\n";
    }

    AssistantMessage result(const std::shared_ptr<AssistantMessageStream>& stream) {
        while (stream->next()) {
        }
        return *stream->result();
    }

    std::string header(const HttpRequest& request, const std::string& name) {
        return m_headers.find(request.headers, name).value_or("");
    }

    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    FixedClock m_clock;
    InlineExecutor m_executor;
    AnthropicMessagesProvider m_provider{m_http, m_sleeper, m_clock, m_executor};
    HeaderMerger m_headers;
};

TEST_F(AnthropicMessagesProviderTest, StreamsTextResponse) {
    m_http.enqueue(sse(helloStream()));
    StreamOptions options;
    options.apiKey = "sk-ant-api03-test";
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    ASSERT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "Hi");
    EXPECT_EQ(message.responseId, "msg_1");
    EXPECT_EQ(message.usage.input, 10);
    EXPECT_EQ(message.usage.output, 5);
    EXPECT_EQ(message.provider, "anthropic");
    ASSERT_EQ(m_http.calls(), 1);
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.url, "https://api.anthropic.com/v1/messages?beta=true");
    EXPECT_EQ(header(request, "x-api-key"), "sk-ant-api03-test");
    EXPECT_EQ(header(request, "anthropic-version"), "2023-06-01");
    EXPECT_EQ(header(request, "content-type"), "application/json");
    const Json body = Json::parse(request.body);
    EXPECT_EQ(body["model"], "claude-sonnet-4");
    EXPECT_EQ(body["stream"], true);
    EXPECT_EQ(body["messages"][0]["content"][0]["text"], "hello");
}

TEST_F(AnthropicMessagesProviderTest, OAuthTokenUsesBearerAndClaudeCodeHeaders) {
    m_http.enqueue(sse(helloStream()));
    StreamOptions options;
    options.apiKey = "sk-ant-oat01-secret";
    result(m_provider.stream(model(), context("hello"), options));
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(header(request, "authorization"), "Bearer sk-ant-oat01-secret");
    EXPECT_EQ(header(request, "x-api-key"), "");
    EXPECT_EQ(header(request, "x-app"), "cli");
    EXPECT_NE(header(request, "anthropic-beta").find("oauth-2025-04-20"), std::string::npos);
}

TEST_F(AnthropicMessagesProviderTest, MissingApiKeyFailsWithoutRequest) {
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), {}));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, "No API key for provider: anthropic");
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(AnthropicMessagesProviderTest, HeaderOwnedAuthIsAccepted) {
    m_http.enqueue(sse(helloStream()));
    StreamOptions options;
    options.headers = {{"x-api-key", "from-header"}};
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(header(m_http.requests()[0], "x-api-key"), "from-header");
}

TEST_F(AnthropicMessagesProviderTest, HttpErrorReportedSdkStyle) {
    HttpResponse failure;
    failure.status = 400;
    failure.body = R"({"type":"error","error":{"type":"invalid_request_error","message":"bad"}})";
    m_http.enqueue(failure);
    StreamOptions options;
    options.apiKey = "k";
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, "400 " + failure.body);
}

TEST_F(AnthropicMessagesProviderTest, RetriesOverloadedThenStreams) {
    HttpResponse overloaded;
    overloaded.status = 529;
    overloaded.headers = {{"retry-after-ms", "5"}};
    m_http.enqueue(overloaded);
    m_http.enqueue(sse(helloStream()));
    StreamOptions options;
    options.apiKey = "k";
    options.maxRetries = 2;
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(m_http.calls(), 2);
    EXPECT_EQ(m_sleeper.delays(), (std::vector<std::int64_t>{5}));
}

TEST_F(AnthropicMessagesProviderTest, OnPayloadCanReplaceBody) {
    m_http.enqueue(sse(helloStream()));
    StreamOptions options;
    options.apiKey = "k";
    options.onPayload = [](const Json& body, const Model&) -> std::optional<Json> {
        Json replaced = body;
        replaced["max_tokens"] = 1;
        return replaced;
    };
    result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(Json::parse(m_http.requests()[0].body)["max_tokens"], 1);
}

TEST_F(AnthropicMessagesProviderTest, TruncatedStreamIsError) {
    std::string body = helloStream();
    body.resize(body.find("event: message_stop"));
    m_http.enqueue(sse(body));
    StreamOptions options;
    options.apiKey = "k";
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, "Anthropic stream ended before message_stop");
}

TEST(AnthropicMessagesProviderEndToEndTest, StreamsOverRealHttp) {
    const std::string events =
        "event: message_start\ndata: "
        R"({"type":"message_start","message":{"id":"m","model":"claude-sonnet-4","usage":{"input_tokens":3}}})"
        "\n\n"
        "event: content_block_start\ndata: "
        R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"
        "\n\n"
        "event: content_block_delta\ndata: "
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"over the wire"}})"
        "\n\n"
        "event: content_block_stop\ndata: "
        R"({"type":"content_block_stop","index":0})"
        "\n\n"
        "event: message_delta\ndata: "
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":4}})"
        "\n\n"
        "event: message_stop\ndata: "
        R"({"type":"message_stop"})"
        "\n\n";
    FakeHttpServer server([&](const FakeHttpRequest&) {
        FakeHttpReply reply;
        reply.headers = {{"Content-Type", "text/event-stream"}};
        // Split mid-event to exercise incremental parsing.
        reply.chunks = {events.substr(0, 37), events.substr(37)};
        return reply;
    });
    CurlHttpClient http;
    RecordingSleeper sleeper;
    FixedClock clock;
    ThreadPool pool(1);
    AnthropicMessagesProvider provider(http, sleeper, clock, pool);
    Model model;
    model.id = "claude-sonnet-4";
    model.api = "anthropic-messages";
    model.provider = "anthropic";
    model.baseUrl = "http://127.0.0.1:" + std::to_string(server.port());
    model.maxTokens = 1000;
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
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "over the wire");
}
