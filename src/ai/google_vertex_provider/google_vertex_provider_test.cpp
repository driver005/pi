#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.google_vertex_provider;
import pi.testing.fake_environment;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class StaticTokens : public IAccessTokenSource {
public:
    Result<std::string> token(const std::map<std::string, std::string>&) override {
        ++m_calls;
        if (m_fail) {
            return std::unexpected(Error{"auth_failed", "no credentials"});
        }
        return std::string("adc-token");
    }

    int calls() const {
        return m_calls;
    }

    void failNext() {
        m_fail = true;
    }

private:
    int m_calls = 0;
    bool m_fail = false;
};

class GoogleVertexProviderTest : public testing::Test {
protected:
    Model model(const std::string& baseUrl = "https://{location}-aiplatform.googleapis.com") {
        Model m;
        m.id = "gemini-2.5-flash";
        m.api = "google-vertex";
        m.provider = "google-vertex";
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
        response.body = R"(data: {"candidates":[{"content":{"parts":[{"text":"Hi"}]},"finishReason":"STOP"}]})"
                        "\n\n";
        return response;
    }

    AssistantMessage result(const std::shared_ptr<AssistantMessageStream>& stream) {
        while (stream->next()) {
        }
        return *stream->result();
    }

    FakeEnvironment m_environment;
    StaticTokens m_tokens;
    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    FixedClock m_clock;
    InlineExecutor m_executor;
    GoogleVertexProvider m_provider{m_http, m_sleeper, m_clock, m_executor, m_environment, m_tokens};
    HeaderMerger m_headers;
};

TEST_F(GoogleVertexProviderTest, AdcModeUsesProjectLocationAndBearerToken) {
    m_environment.set("GOOGLE_CLOUD_PROJECT", "proj");
    m_environment.set("GOOGLE_CLOUD_LOCATION", "us-central1");
    m_http.enqueue(reply());
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), {}));
    ASSERT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(message.api, "google-vertex");
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url,
              "https://us-central1-aiplatform.googleapis.com/v1/projects/proj/locations/us-central1/publishers/"
              "google/models/gemini-2.5-flash:streamGenerateContent?alt=sse");
    EXPECT_EQ(m_headers.find(request.headers, "authorization"), "Bearer adc-token");
}

TEST_F(GoogleVertexProviderTest, GlobalLocationUsesTheGlobalHost) {
    StreamOptions options;
    options.env = {{"GOOGLE_CLOUD_PROJECT", "p"}, {"GOOGLE_CLOUD_LOCATION", "global"}};
    m_http.enqueue(reply());
    result(m_provider.stream(model(), context("hello"), options));
    EXPECT_EQ(m_http.requests()[0].url.rfind("https://aiplatform.googleapis.com/v1/projects/p/locations/global/", 0), 0U);
}

TEST_F(GoogleVertexProviderTest, ApiKeyUsesTheExpressEndpoint) {
    StreamOptions options;
    options.apiKey = "vertex-key";
    m_http.enqueue(reply());
    result(m_provider.stream(model(), context("hello"), options));
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url,
              "https://aiplatform.googleapis.com/v1/publishers/google/models/gemini-2.5-flash:streamGenerateContent?alt=sse");
    EXPECT_EQ(m_headers.find(request.headers, "x-goog-api-key"), "vertex-key");
    EXPECT_FALSE(m_headers.find(request.headers, "authorization").has_value());
    EXPECT_EQ(m_tokens.calls(), 0);
}

TEST_F(GoogleVertexProviderTest, PlaceholderAndMarkerKeysFallBackToAdc) {
    m_environment.set("GOOGLE_CLOUD_PROJECT", "p");
    m_environment.set("GOOGLE_CLOUD_LOCATION", "us-east1");
    for (const std::string key : {"gcp-vertex-credentials", "<authenticated>", "  "}) {
        StreamOptions options;
        options.apiKey = key;
        m_http.enqueue(reply());
        result(m_provider.stream(model(), context("hello"), options));
    }
    EXPECT_EQ(m_tokens.calls(), 3);
}

TEST_F(GoogleVertexProviderTest, CustomBaseUrlIsCollectionScoped) {
    m_http.enqueue(reply());
    StreamOptions options;
    options.apiKey = "k";
    result(m_provider.stream(model("https://proxy.example.com/vertex/"), context("hello"), options));
    EXPECT_EQ(m_http.requests()[0].url,
              "https://proxy.example.com/vertex/v1/publishers/google/models/gemini-2.5-flash:streamGenerateContent?alt=sse");
    m_http.enqueue(reply());
    result(m_provider.stream(model("https://proxy.example.com/v1beta1"), context("hello"), options));
    EXPECT_EQ(m_http.requests()[1].url,
              "https://proxy.example.com/v1beta1/publishers/google/models/gemini-2.5-flash:streamGenerateContent?alt=sse");
}

TEST_F(GoogleVertexProviderTest, MissingProjectOrLocationOrTokenFailsWithoutRequest) {
    const AssistantMessage noProject = result(m_provider.stream(model(), context("hello"), {}));
    EXPECT_NE(noProject.errorMessage->find("requires a project ID"), std::string::npos);
    m_environment.set("GOOGLE_CLOUD_PROJECT", "p");
    const AssistantMessage noLocation = result(m_provider.stream(model(), context("hello"), {}));
    EXPECT_NE(noLocation.errorMessage->find("requires a location"), std::string::npos);
    m_environment.set("GOOGLE_CLOUD_LOCATION", "us-central1");
    m_tokens.failNext();
    const AssistantMessage noToken = result(m_provider.stream(model(), context("hello"), {}));
    EXPECT_EQ(noToken.errorMessage, "no credentials");
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(GoogleVertexProviderTest, PublisherQualifiedModelIds) {
    Model m = model();
    m.id = "meta/llama-4";
    StreamOptions options;
    options.apiKey = "k";
    m_http.enqueue(reply());
    result(m_provider.stream(m, context("hello"), options));
    EXPECT_NE(m_http.requests()[0].url.find("/publishers/meta/models/llama-4:"), std::string::npos);
}
