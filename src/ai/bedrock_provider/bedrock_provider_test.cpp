#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.bedrock_provider;
import pi.base.base64_codec;
import pi.base.boring_crypto;
import pi.support.aws_event_stream_parser;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class BedrockProviderTest : public testing::Test {
protected:
    Model model() {
        Model m;
        m.id = "anthropic.claude-3-5-haiku-20241022-v1:0";
        m.name = "Claude 3.5 Haiku";
        m.api = "bedrock-converse-stream";
        m.provider = "amazon-bedrock";
        m.baseUrl = "https://bedrock-runtime.us-east-1.amazonaws.com";
        m.maxTokens = 4000;
        m.contextWindow = 200000;
        return m;
    }

    TranscriptContext context(const std::string& prompt) {
        UserMessage user;
        user.content = prompt;
        TranscriptContext c;
        c.messages.emplace_back(user);
        return c;
    }

    void putUint32(std::string& out, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((value >> shift) & 0xFF));
        }
    }

    std::string frame(const std::string& type, const std::string& payload) {
        std::string headers;
        for (const auto& pair : std::vector<std::pair<std::string, std::string>>{{":message-type", "event"}, {":event-type", type}}) {
            headers.push_back(static_cast<char>(pair.first.size()));
            headers += pair.first;
            headers.push_back(7);
            headers.push_back(static_cast<char>(pair.second.size() >> 8));
            headers.push_back(static_cast<char>(pair.second.size() & 0xFF));
            headers += pair.second;
        }
        std::string out;
        putUint32(out, static_cast<std::uint32_t>(16 + headers.size() + payload.size()));
        putUint32(out, static_cast<std::uint32_t>(headers.size()));
        putUint32(out, m_parser.crc32(out));
        out += headers + payload;
        putUint32(out, m_parser.crc32(out));
        return out;
    }

    HttpResponse reply() {
        HttpResponse response;
        response.status = 200;
        response.body = frame("messageStart", R"({"role":"assistant"})") +
                        frame("contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"text":"Hi"}})") +
                        frame("contentBlockStop", R"({"contentBlockIndex":0})") +
                        frame("messageStop", R"({"stopReason":"end_turn"})") +
                        frame("metadata", R"({"usage":{"inputTokens":10,"outputTokens":3,"totalTokens":13}})");
        return response;
    }

    AssistantMessage result(const std::shared_ptr<AssistantMessageStream>& stream) {
        while (stream->next()) {
        }
        return *stream->result();
    }

    void useAccessKeys() {
        m_environment.set("AWS_ACCESS_KEY_ID", "AKID");
        m_environment.set("AWS_SECRET_ACCESS_KEY", "secret");
        m_environment.set("AWS_REGION", "us-west-2");
    }

    AwsEventStreamParser m_parser;
    FakeEnvironment m_environment;
    FakeFileSystem m_files;
    BoringCrypto m_crypto;
    Base64Codec m_base64;
    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    FixedClock m_clock{1440938160000};
    InlineExecutor m_executor;
    BedrockProvider m_provider{m_http, m_sleeper, m_clock, m_executor, m_environment, m_files, m_crypto, m_base64};
    HeaderMerger m_headers;
};

TEST_F(BedrockProviderTest, SignsRequestsAndStreamsTheResponse) {
    useAccessKeys();
    m_http.enqueue(reply());
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), {}));
    ASSERT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(message.api, "bedrock-converse-stream");
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "Hi");
    EXPECT_EQ(message.usage.totalTokens, 13);
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://bedrock-runtime.us-west-2.amazonaws.com/model/anthropic.claude-3-5-haiku-20241022-v1%3A0/converse-stream");
    const std::string auth = *m_headers.find(request.headers, "authorization");
    EXPECT_NE(auth.find("AWS4-HMAC-SHA256 Credential=AKID/20150830/us-west-2/bedrock/aws4_request"), std::string::npos);
    EXPECT_NE(auth.find("SignedHeaders=content-type;host;x-amz-date"), std::string::npos);
    EXPECT_EQ(m_headers.find(request.headers, "x-amz-date"), "20150830T123600Z");
    EXPECT_EQ(m_headers.find(request.headers, "accept"), "application/vnd.amazon.eventstream");
    const Json body = Json::parse(request.body);
    EXPECT_EQ(body["messages"][0]["content"][0]["text"], "hello");
    EXPECT_EQ(body["inferenceConfig"]["maxTokens"], 4000);
}

TEST_F(BedrockProviderTest, BearerTokenSkipsSigning) {
    m_http.enqueue(reply());
    StreamOptions options;
    options.apiKey = "bedrock-api-key";
    result(m_provider.stream(model(), context("hello"), options));
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(m_headers.find(request.headers, "authorization"), "Bearer bedrock-api-key");
    EXPECT_FALSE(m_headers.find(request.headers, "x-amz-date").has_value());
}

TEST_F(BedrockProviderTest, CustomHeadersPassButReservedOnesDoNot) {
    useAccessKeys();
    m_http.enqueue(reply());
    StreamOptions options;
    options.headers = {{"x-team", "ml"}, {"X-Amz-Date", "forged"}, {"Authorization", "forged"}};
    result(m_provider.stream(model(), context("hello"), options));
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(m_headers.find(request.headers, "x-team"), "ml");
    EXPECT_NE(m_headers.find(request.headers, "x-amz-date"), "forged");
    EXPECT_NE(m_headers.find(request.headers, "authorization"), "forged");
    EXPECT_NE(m_headers.find(request.headers, "authorization")->find("x-team"), std::string::npos);
}

TEST_F(BedrockProviderTest, MissingCredentialsFailWithoutRequest) {
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), {}));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_NE(message.errorMessage->find("AWS credentials"), std::string::npos);
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(BedrockProviderTest, HttpErrorsGetReadablePrefixesAndDiagnostics) {
    useAccessKeys();
    HttpResponse failure;
    failure.status = 400;
    failure.headers = {{"x-amzn-ErrorType", "ValidationException:http://internal.amazon.com/"}, {"x-amzn-RequestId", "req-1"}};
    failure.body = R"({"message":"Input is too long. Data retention mode 'default' is not available"})";
    m_http.enqueue(failure);
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), {}));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage->rfind("Validation error: Input is too long.", 0), 0U);
    EXPECT_NE(message.errorMessage->find("data-retention.html"), std::string::npos);
    ASSERT_TRUE(message.diagnostics.is_array());
    EXPECT_EQ(message.diagnostics[0]["details"]["status"], 400);
    EXPECT_EQ(message.diagnostics[0]["details"]["errorCode"], "ValidationException");
    EXPECT_EQ(message.diagnostics[0]["details"]["requestId"], "req-1");
}

TEST_F(BedrockProviderTest, UntypedErrorsShowTheStatus) {
    useAccessKeys();
    HttpResponse failure;
    failure.status = 502;
    failure.body = "bad gateway";
    m_http.enqueue(failure);
    EXPECT_EQ(result(m_provider.stream(model(), context("hello"), {})).errorMessage, "502: bad gateway");
}

TEST_F(BedrockProviderTest, StreamExceptionsAreReported) {
    useAccessKeys();
    HttpResponse response;
    response.status = 200;
    // An exception frame: :message-type exception, :exception-type throttlingException.
    std::string headers;
    for (const auto& pair : std::vector<std::pair<std::string, std::string>>{{":message-type", "exception"}, {":exception-type", "throttlingException"}}) {
        headers.push_back(static_cast<char>(pair.first.size()));
        headers += pair.first;
        headers.push_back(7);
        headers.push_back(0);
        headers.push_back(static_cast<char>(pair.second.size()));
        headers += pair.second;
    }
    const std::string payload = R"({"message":"Too many requests"})";
    std::string out;
    putUint32(out, static_cast<std::uint32_t>(16 + headers.size() + payload.size()));
    putUint32(out, static_cast<std::uint32_t>(headers.size()));
    putUint32(out, m_parser.crc32(out));
    out += headers + payload;
    putUint32(out, m_parser.crc32(out));
    response.body = out;
    m_http.enqueue(response);
    const AssistantMessage message = result(m_provider.stream(model(), context("hello"), {}));
    EXPECT_EQ(message.errorMessage, "Throttling error: Too many requests");
}
