#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.mistral_provider;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class MistralProviderTest : public testing::Test {
protected:
    Model model(const std::string& baseUrl = "https://api.mistral.ai") {
        Model m;
        m.id = "mistral-large";
        m.api = "mistral-conversations";
        m.provider = "mistral";
        m.baseUrl = baseUrl;
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
        response.body = R"(data: {"id":"c1","choices":[{"delta":{"content":"Hi"},"finish_reason":"stop"}],)"
                        R"("usage":{"prompt_tokens":10,"completion_tokens":3,"total_tokens":13}})"
                        "\n\n"
                        "data: [DONE]\n\n";
        return response;
    }

    AssistantMessage result(const std::shared_ptr<AssistantMessageStream>& stream) {
        while (stream->next()) {
        }
        return *stream->result();
    }

    StreamOptions keyed() {
        StreamOptions options;
        options.apiKey = "m-key";
        return options;
    }

    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    FixedClock m_clock;
    InlineExecutor m_executor;
    MistralProvider m_provider{m_http, m_sleeper, m_clock, m_executor};
    HeaderMerger m_headers;
};

TEST_F(MistralProviderTest, StreamsTextResponse) {
    m_http.enqueue(reply());
    StreamOptions options = keyed();
    options.sessionId = "sess";
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), options));
    ASSERT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "Hi");
    EXPECT_EQ(message.usage.input, 10);
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://api.mistral.ai/v1/chat/completions");
    EXPECT_EQ(m_headers.find(request.headers, "authorization"), "Bearer m-key");
    EXPECT_EQ(m_headers.find(request.headers, "x-affinity"), "sess");
    EXPECT_EQ(Json::parse(request.body)["prompt_cache_key"], "sess");
    EXPECT_EQ(request.idleTimeout, std::chrono::milliseconds(60000));
}

TEST_F(MistralProviderTest, ExplicitAffinityHeaderWins) {
    m_http.enqueue(reply());
    StreamOptions options = keyed();
    options.sessionId = "sess";
    options.headers = {{"X-Affinity", "mine"}};
    result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(m_headers.find(m_http.requests()[0].headers, "x-affinity"), "mine");
}

TEST_F(MistralProviderTest, BaseUrlWithPathAndTimeoutOverride) {
    m_http.enqueue(reply());
    StreamOptions options = keyed();
    options.timeoutMs = 5000;
    result(m_provider.stream(model("https://proxy.example.com/mistral/"), context("hello"), options));
    EXPECT_EQ(m_http.requests()[0].url, "https://proxy.example.com/mistral/v1/chat/completions");
    EXPECT_EQ(m_http.requests()[0].idleTimeout, std::chrono::milliseconds(5000));
}

TEST_F(MistralProviderTest, MissingKeyFailsWithoutRequest) {
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), {}));
    EXPECT_EQ(message.errorMessage, "No API key for provider: mistral");
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(MistralProviderTest, HttpErrorIncludesStatusAndBody) {
    HttpResponse failure;
    failure.status = 401;
    failure.body = R"(  {"message":"Unauthorized"}  )";
    m_http.enqueue(failure);
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), keyed()));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, R"(Mistral API error (401): {"message":"Unauthorized"})");
}

TEST_F(MistralProviderTest, EmptyErrorBodyFallsBackToTheStatus) {
    HttpResponse failure;
    failure.status = 403;
    m_http.enqueue(failure);
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), keyed()));
    EXPECT_EQ(message.errorMessage, "Mistral API error (403): Request failed with status 403");
}
