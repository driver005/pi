#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.pi_messages_provider;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class PiMessagesProviderTest : public testing::Test {
protected:
    Model model() {
        Model m;
        m.id = "gpt-x";
        m.api = "pi-messages";
        m.provider = "radius";
        m.baseUrl = "https://radius.example.com/v1/";
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
        response.body = R"(data: {"type":"start"})"
                        "\n\n"
                        R"(data: {"type":"text_start","contentIndex":0})"
                        "\n\n"
                        R"(data: {"type":"text_delta","contentIndex":0,"delta":"Hi"})"
                        "\n\n"
                        R"(data: {"type":"text_end","contentIndex":0,"content":"Hi"})"
                        "\n\n"
                        R"(data: {"type":"done","reason":"stop","usage":{"input":3,"output":1,"cacheRead":0,"cacheWrite":0,)"
                        R"("totalTokens":4,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}}})"
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
        options.apiKey = "rk";
        return options;
    }

    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    FixedClock m_clock{7000};
    InlineExecutor m_executor;
    PiMessagesProvider m_provider{m_http, m_sleeper, m_clock, m_executor};
    HeaderMerger m_headers;
};

TEST_F(PiMessagesProviderTest, SendsTheContextAndReplaysEvents) {
    m_http.enqueue(reply());
    StreamOptions options = keyed();
    options.temperature = 0.5;
    options.maxTokens = 100;
    options.reasoning = ThinkingLevel::High;
    options.sessionId = "s";
    options.toolChoice = "auto";
    options.cacheRetention = "long";
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    ASSERT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "Hi");
    EXPECT_EQ(message.usage.totalTokens, 4);
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://radius.example.com/v1/messages");
    EXPECT_EQ(m_headers.find(request.headers, "authorization"), "Bearer rk");
    const Json body = Json::parse(request.body);
    EXPECT_EQ(body["model"], "gpt-x");
    EXPECT_EQ(body["context"]["messages"][0]["role"], "user");
    EXPECT_EQ(body["context"]["messages"][0]["content"], "hello");
    EXPECT_EQ(body["options"], (Json{{"temperature", 0.5},
                                     {"maxTokens", 100},
                                     {"reasoning", "high"},
                                     {"cacheRetention", "long"},
                                     {"sessionId", "s"},
                                     {"toolChoice", "auto"}}));
}

TEST_F(PiMessagesProviderTest, UnsetOptionsAreOmittedAndLegacyEnvMapsToLongRetention) {
    m_http.enqueue(reply());
    StreamOptions options = keyed();
    options.env["PI_CACHE_RETENTION"] = "long";
    result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(Json::parse(m_http.requests()[0].body)["options"], (Json{{"cacheRetention", "long"}}));
}

TEST_F(PiMessagesProviderTest, MissingKeyFailsWithoutRequest) {
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), {}));
    EXPECT_EQ(message.errorMessage, "No API key provided for provider \"radius\"");
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(PiMessagesProviderTest, HttpErrorsCarryDiagnostics) {
    HttpResponse failure;
    failure.status = 429;
    failure.body = R"({"error":{"message":"slow down","code":"rate_limited","retryAfter":3}})";
    m_http.enqueue(failure);
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), keyed()));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, "429: slow down (rate_limited)");
    ASSERT_TRUE(message.diagnostics.is_array());
    const Json& diagnostic = message.diagnostics[0];
    EXPECT_EQ(diagnostic["type"], "pi_messages_response_failure");
    EXPECT_EQ(diagnostic["error"]["code"], "rate_limited");
    EXPECT_EQ(diagnostic["details"]["status"], 429);
    EXPECT_EQ(diagnostic["details"]["url"], "https://radius.example.com/v1/messages");
    EXPECT_EQ(diagnostic["details"]["error"]["retryAfter"], 3);
}

TEST_F(PiMessagesProviderTest, UnstructuredErrorBodiesAreKept) {
    HttpResponse failure;
    failure.status = 502;
    failure.body = "Bad gateway";
    m_http.enqueue(failure);
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), keyed()));
    EXPECT_EQ(message.errorMessage, "502: Bad gateway");
    EXPECT_EQ(message.diagnostics[0]["details"]["body"], "Bad gateway");
}

TEST_F(PiMessagesProviderTest, StreamWithoutTerminalEventFails) {
    HttpResponse partial;
    partial.status = 200;
    partial.body = R"(data: {"type":"start"})"
                   "\n\n";
    m_http.enqueue(partial);
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), keyed()));
    EXPECT_EQ(message.errorMessage, "radius stream ended without a terminal event");
}

TEST_F(PiMessagesProviderTest, BackendErrorEventIsReportedAsIs) {
    HttpResponse failed;
    failed.status = 200;
    failed.body = R"(data: {"type":"error","reason":"error","errorMessage":"blocked","usage":{"input":0,"output":0,"cacheRead":0,)"
                  R"("cacheWrite":0,"totalTokens":0,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}}})"
                  "\n\n";
    m_http.enqueue(failed);
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), keyed()));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, "blocked");
}
