#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.azure_responses_provider;
import pi.testing.fake_environment;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class AzureResponsesProviderTest : public testing::Test {
protected:
    Model model(const std::string& baseUrl = "https://res.openai.azure.com") {
        Model m;
        m.id = "gpt-test";
        m.api = "azure-openai-responses";
        m.provider = "azure-openai-responses";
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
        response.body =
            R"(data: {"type":"response.output_item.added","output_index":0,"item":{"type":"message","id":"m"}})"
            "\n\n"
            R"(data: {"type":"response.output_item.done","output_index":0,"item":{"type":"message","id":"m","content":[{"type":"output_text","text":"ok"}]}})"
            "\n\n"
            R"(data: {"type":"response.completed","response":{"status":"completed"}})"
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
        options.apiKey = "azure-key";
        return options;
    }

    FakeEnvironment m_environment;
    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    FixedClock m_clock;
    InlineExecutor m_executor;
    AzureResponsesProvider m_provider{m_http, m_sleeper, m_clock, m_executor, m_environment};
    HeaderMerger m_headers;
};

TEST_F(AzureResponsesProviderTest, SendsApiKeyHeaderAndDefaultVersion) {
    m_http.enqueue(reply());
    const AssistantMessage message = result(m_provider.stream(model(), context("hi"), keyed()));
    ASSERT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(message.api, "azure-openai-responses");
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://res.openai.azure.com/openai/v1/responses?api-version=v1");
    EXPECT_EQ(m_headers.find(request.headers, "api-key"), "azure-key");
    EXPECT_FALSE(m_headers.find(request.headers, "authorization").has_value());
    const Json body = Json::parse(request.body);
    EXPECT_EQ(body["model"], "gpt-test");
    EXPECT_FALSE(body.contains("prompt_cache_retention"));
}

TEST_F(AzureResponsesProviderTest, DeploymentMapAndVersionFromEnvironment) {
    m_environment.set("AZURE_OPENAI_DEPLOYMENT_NAME_MAP", "other=x, gpt-test = my-deploy");
    m_environment.set("AZURE_OPENAI_API_VERSION", "2025-04-01");
    m_http.enqueue(reply());
    result(m_provider.stream(model(), context("hi"), keyed()));
    const HttpRequest request = m_http.requests()[0];
    EXPECT_NE(request.url.find("api-version=2025-04-01"), std::string::npos);
    EXPECT_EQ(Json::parse(request.body)["model"], "my-deploy");
}

TEST_F(AzureResponsesProviderTest, ProviderEnvironmentBeatsProcessEnvironment) {
    m_environment.set("AZURE_OPENAI_RESOURCE_NAME", "processres");
    StreamOptions options = keyed();
    options.env["AZURE_OPENAI_RESOURCE_NAME"] = "optres";
    m_http.enqueue(reply());
    result(m_provider.stream(model(""), context("hi"), options));
    EXPECT_EQ(m_http.requests()[0].url, "https://optres.openai.azure.com/openai/v1/responses?api-version=v1");
}

TEST_F(AzureResponsesProviderTest, BaseUrlNormalizationKeepsCustomHosts) {
    m_http.enqueue(reply());
    result(m_provider.stream(model("https://proxy.example.com/azure/"), context("hi"), keyed()));
    EXPECT_EQ(m_http.requests()[0].url, "https://proxy.example.com/azure/responses?api-version=v1");
    m_http.enqueue(reply());
    result(m_provider.stream(model("https://res.cognitiveservices.azure.com/openai/v1/responses"), context("hi"),
                             keyed()));
    EXPECT_EQ(m_http.requests()[1].url, "https://res.cognitiveservices.azure.com/openai/v1/responses?api-version=v1");
}

TEST_F(AzureResponsesProviderTest, MissingKeyOrBaseUrlFailsWithoutRequest) {
    EXPECT_EQ(result(m_provider.stream(model(), context("hi"), {})).errorMessage,
              "No API key for provider: azure-openai-responses");
    const AssistantMessage noBase = result(m_provider.stream(model(""), context("hi"), keyed()));
    EXPECT_EQ(noBase.stopReason, StopReason::Error);
    EXPECT_NE(noBase.errorMessage->find("base URL is required"), std::string::npos);
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(AzureResponsesProviderTest, ToolsDefaultToStrictFalse) {
    Tool tool;
    tool.name = "read";
    tool.description = "d";
    tool.parameters = Json::parse(R"({"type":"object","properties":{}})");
    Context raw;
    raw.systemPrompt = "sys";
    raw.tools = std::vector<Tool>{tool};
    UserMessage user;
    user.content = "hi";
    raw.messages.emplace_back(user);
    TranscriptNormalizer normalizer;
    m_http.enqueue(reply());
    result(m_provider.stream(model(), normalizer.normalizeContext(raw), keyed()));
    EXPECT_EQ(Json::parse(m_http.requests()[0].body)["tools"][0]["strict"], false);
}
