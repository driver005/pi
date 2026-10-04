#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.codex_provider;
import pi.base.base64_codec;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class CodexProviderTest : public testing::Test {
protected:
    Model model(const std::string& baseUrl = "") {
        Model m;
        m.id = "gpt-5-codex";
        m.api = "openai-codex-responses";
        m.provider = "openai-codex";
        m.baseUrl = baseUrl;
        m.input = {"text"};
        return m;
    }

    std::string token(const std::string& account = "acct-1") {
        const Json claims = Json{{"https://api.openai.com/auth", Json{{"chatgpt_account_id", account}}}};
        return "h." + m_base64.encodeUrl(claims.dump()) + ".s";
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
            R"(data: {"type":"response.done","response":{"status":"completed","end_turn":true}})"
            "\n\n";
        return response;
    }

    AssistantMessage result(const std::shared_ptr<AssistantMessageStream>& stream) {
        while (stream->next()) {
        }
        return *stream->result();
    }

    StreamOptions keyed(const std::string& key) {
        StreamOptions options;
        options.apiKey = key;
        return options;
    }

    Base64Codec m_base64;
    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    FixedClock m_clock{1'700'000'000'000};
    InlineExecutor m_executor;
    CodexProvider m_provider{m_http, m_sleeper, m_clock, m_executor, m_base64};
    HeaderMerger m_headers;
};

TEST_F(CodexProviderTest, StreamsWithAccountHeaders) {
    m_http.enqueue(reply());
    StreamOptions options = keyed(token());
    options.sessionId = "sess";
    const AssistantMessage message = result(m_provider.stream(model(), context("hi"), options));
    ASSERT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(message.api, "openai-codex-responses");
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "ok");
    EXPECT_EQ(message.endTurn, true);
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://chatgpt.com/backend-api/codex/responses");
    EXPECT_EQ(m_headers.find(request.headers, "chatgpt-account-id"), "acct-1");
    EXPECT_EQ(m_headers.find(request.headers, "authorization"), "Bearer " + token());
    EXPECT_EQ(m_headers.find(request.headers, "originator"), "pi");
    EXPECT_EQ(m_headers.find(request.headers, "openai-beta"), "responses=experimental");
    EXPECT_EQ(m_headers.find(request.headers, "session-id"), "sess");
    EXPECT_EQ(m_headers.find(request.headers, "x-client-request-id"), "sess");
    EXPECT_EQ(Json::parse(request.body)["model"], "gpt-5-codex");
}

TEST_F(CodexProviderTest, UrlResolution) {
    for (const std::string& base : {"https://x.example/backend-api/", "https://x.example/backend-api/codex",
                                    "https://x.example/backend-api/codex/responses"}) {
        m_http.enqueue(reply());
        result(m_provider.stream(model(base), context("hi"), keyed(token())));
    }
    for (const auto& request : m_http.requests()) {
        EXPECT_EQ(request.url, "https://x.example/backend-api/codex/responses");
    }
}

TEST_F(CodexProviderTest, BadTokensAndMissingKeys) {
    EXPECT_EQ(result(m_provider.stream(model(), context("hi"), {})).errorMessage, "No API key for provider: openai-codex");
    EXPECT_EQ(result(m_provider.stream(model(), context("hi"), keyed("not.a.jwt.at.all"))).errorMessage,
              "Failed to extract accountId from token");
    const std::string noClaim = "h." + m_base64.encodeUrl("{}") + ".s";
    EXPECT_EQ(result(m_provider.stream(model(), context("hi"), keyed(noClaim))).errorMessage,
              "Failed to extract accountId from token");
    EXPECT_EQ(m_http.calls(), 0);
}

TEST_F(CodexProviderTest, UsageLimitErrorsAreFriendly) {
    HttpResponse failure;
    failure.status = 429;
    const std::int64_t resets = 1'700'000'000 + 600;
    failure.body = Json{{"error", Json{{"code", "usage_limit_reached"}, {"plan_type", "PLUS"}, {"resets_at", resets}}}}.dump();
    m_http.enqueue(failure);
    const AssistantMessage message = result(m_provider.stream(model(), context("hi"), keyed(token())));
    EXPECT_EQ(message.errorMessage, "You have hit your ChatGPT usage limit (plus plan). Try again in ~10 min.");
}

TEST_F(CodexProviderTest, ServerMessageWinsOtherwiseTheBody) {
    HttpResponse failure;
    failure.status = 400;
    failure.body = R"({"error":{"message":"bad request body"}})";
    m_http.enqueue(failure);
    EXPECT_EQ(result(m_provider.stream(model(), context("hi"), keyed(token()))).errorMessage, "bad request body");
    HttpResponse plain;
    plain.status = 502;
    plain.body = "upstream down";
    m_http.enqueue(plain);
    EXPECT_EQ(result(m_provider.stream(model(), context("hi"), keyed(token()))).errorMessage, "upstream down");
    HttpResponse empty;
    empty.status = 500;
    m_http.enqueue(empty);
    EXPECT_EQ(result(m_provider.stream(model(), context("hi"), keyed(token()))).errorMessage, "Request failed");
}

TEST_F(CodexProviderTest, ErrorEventsAreReported) {
    HttpResponse failed;
    failed.status = 200;
    failed.body = R"(data: {"type":"error","code":"x","message":"nope"})"
                  "\n\n";
    m_http.enqueue(failed);
    const AssistantMessage message = result(m_provider.stream(model(), context("hi"), keyed(token())));
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, "Codex error: nope");
}
