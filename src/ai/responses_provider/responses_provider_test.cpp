#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.responses_provider;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class ResponsesProviderTest : public testing::Test {
protected:
    Model model() {
        Model m;
        m.id = "gpt-test";
        m.api = "openai-responses";
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
        return "event: response.created\n"
               R"(data: {"type":"response.created","response":{"id":"resp_1"}})"
               "\n\n"
               R"(data: {"type":"response.output_item.added","output_index":0,"item":{"type":"message","id":"msg_1"}})"
               "\n\n"
               R"(data: {"type":"response.output_text.delta","output_index":0,"delta":"Hi"})"
               "\n\n"
               R"(data: {"type":"response.output_item.done","output_index":0,"item":{"type":"message","id":"msg_1","content":[{"type":"output_text","text":"Hi"}]}})"
               "\n\n"
               R"(data: {"type":"response.completed","response":{"id":"resp_1","status":"completed","usage":{"input_tokens":10,"output_tokens":3,"total_tokens":13}}})"
               "\n\n";
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
    ResponsesProvider m_provider{m_http, m_sleeper, m_clock, m_executor};
    HeaderMerger m_headers;
};

TEST_F(ResponsesProviderTest, StreamsTextResponse) {
    m_http.enqueue(sse(helloStream()));
    StreamOptions options;
    options.apiKey = "sk-test";
    options.sessionId = "sess-1";
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    ASSERT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "Hi");
    EXPECT_EQ(message.usage.input, 10);
    EXPECT_EQ(message.usage.output, 3);
    EXPECT_EQ(message.responseId, "resp_1");
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://api.openai.com/v1/responses");
    EXPECT_EQ(m_headers.find(request.headers, "authorization"), "Bearer sk-test");
    EXPECT_EQ(m_headers.find(request.headers, "session_id"), "sess-1");
    EXPECT_EQ(m_headers.find(request.headers, "x-client-request-id"), "sess-1");
    const Json body = Json::parse(request.body);
    EXPECT_EQ(body["model"], "gpt-test");
    EXPECT_EQ(body["input"][0]["content"][0]["text"], "hello");
    EXPECT_EQ(body["prompt_cache_key"], "sess-1");
}

TEST_F(ResponsesProviderTest, NoSessionHeadersWhenCachingIsOff) {
    m_http.enqueue(sse(helloStream()));
    StreamOptions options;
    options.apiKey = "sk-test";
    options.sessionId = "sess-1";
    options.cacheRetention = "none";
    result(m_provider.stream(model(), context("hello"), options));
    const HttpRequest request = m_http.requests()[0];
    EXPECT_FALSE(m_headers.find(request.headers, "session_id").has_value());
    EXPECT_FALSE(m_headers.find(request.headers, "x-client-request-id").has_value());
}

TEST_F(ResponsesProviderTest, MissingKeyFailsWithoutRequest) {
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), {}));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, "No API key for provider: openai");
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(ResponsesProviderTest, HeaderOwnedAuthKeepsCallerAuthorization) {
    m_http.enqueue(sse(helloStream()));
    StreamOptions options;
    options.headers = {{"Authorization", "Bearer from-header"}};
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(m_headers.find(m_http.requests()[0].headers, "authorization"), "Bearer from-header");
}

TEST_F(ResponsesProviderTest, HttpErrorReportedOpenAiStyle) {
    HttpResponse failure;
    failure.status = 401;
    failure.body = R"({"error":{"message":"Incorrect API key"}})";
    m_http.enqueue(failure);
    StreamOptions options;
    options.apiKey = "bad";
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_NE(message.errorMessage->find("Incorrect API key"), std::string::npos);
}

TEST_F(ResponsesProviderTest, ChatGptUsageLimitGetsAHint) {
    HttpResponse failure;
    failure.status = 429;
    failure.body = R"({"error":{"message":"limit","type":"subscription_sharing_usage_limit_exceeded"}})";
    m_http.enqueue(failure);
    StreamOptions options;
    options.apiKey = "token";
    options.maxRetries = 0;
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    EXPECT_NE(message.errorMessage->find("Check your ChatGPT usage"), std::string::npos);
}

TEST_F(ResponsesProviderTest, FailedResponseBecomesError) {
    m_http.enqueue(sse(R"(data: {"type":"response.failed","response":{"error":{"code":"server_error","message":"boom"}}})"
                       "\n\n"));
    StreamOptions options;
    options.apiKey = "sk-test";
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, "server_error: boom");
}

TEST_F(ResponsesProviderTest, OnPayloadCanReplaceTheBody) {
    m_http.enqueue(sse(helloStream()));
    StreamOptions options;
    options.apiKey = "sk-test";
    options.onPayload = [](const Json& body, const Model&) -> std::optional<Json> {
        Json copy = body;
        copy["metadata"] = Json{{"x", "y"}};
        return copy;
    };
    result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(Json::parse(m_http.requests()[0].body)["metadata"]["x"], "y");
}
