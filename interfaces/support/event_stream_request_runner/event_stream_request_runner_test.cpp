#include <gtest/gtest.h>

import std;
import pi.support.event_stream_request_runner;
import pi.testing.fixed_clock;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class EventStreamRequestRunnerTest : public testing::Test {
protected:
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

    AssistantStreamEmitter makeEmitter() {
        AssistantMessage message;
        message.stopReason = StopReason::Pending;
        return AssistantStreamEmitter(message);
    }

    AwsEventStreamParser m_parser;
    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    FixedClock m_clock;
    RetryingHttpSender m_sender{m_http, m_sleeper, m_clock};
    EventStreamRequestRunner m_runner{m_sender};
};

TEST_F(EventStreamRequestRunnerTest, DeliversMessagesThenFinishes) {
    HttpResponse reply;
    reply.status = 200;
    reply.body = frame("a", "1") + frame("b", "2");
    m_http.enqueue(reply);
    AssistantStreamEmitter emitter = makeEmitter();
    std::vector<std::string> seen;
    bool finished = false;
    m_runner.run(
        HttpRequest{}, Model{}, StreamOptions{}, emitter,
        [&](const AwsEventStreamMessage& message) {
            seen.push_back(message.headers.at(":event-type") + message.payload);
            return Result<void>{};
        },
        [&]() {
            finished = true;
            emitter.done(StopReason::Stop);
            return Result<void>{};
        },
        [](const HttpResponse&) { return std::string("unused"); });
    EXPECT_EQ(seen, (std::vector<std::string>{"a1", "b2"}));
    EXPECT_TRUE(finished);
    EXPECT_EQ(emitter.message().stopReason, StopReason::Stop);
}

TEST_F(EventStreamRequestRunnerTest, HandlerErrorsAndHttpErrorsBecomeErrorEvents) {
    HttpResponse reply;
    reply.status = 200;
    reply.body = frame("a", "1");
    m_http.enqueue(reply);
    AssistantStreamEmitter first = makeEmitter();
    m_runner.run(
        HttpRequest{}, Model{}, StreamOptions{}, first,
        [](const AwsEventStreamMessage&) { return Result<void>(std::unexpected(Error{"x", "handler failed"})); },
        []() { return Result<void>{}; }, [](const HttpResponse&) { return std::string(); });
    EXPECT_EQ(first.message().errorMessage, "handler failed");

    HttpResponse failure;
    failure.status = 403;
    m_http.enqueue(failure);
    AssistantStreamEmitter second = makeEmitter();
    m_runner.run(
        HttpRequest{}, Model{}, StreamOptions{}, second, [](const AwsEventStreamMessage&) { return Result<void>{}; },
        []() { return Result<void>{}; }, [](const HttpResponse& response) { return "status " + std::to_string(response.status); });
    EXPECT_EQ(second.message().errorMessage, "status 403");
}

TEST_F(EventStreamRequestRunnerTest, CorruptAndTruncatedStreamsFail) {
    HttpResponse corrupt;
    corrupt.status = 200;
    corrupt.body = frame("a", "1");
    corrupt.body[corrupt.body.size() - 1] = static_cast<char>(corrupt.body.back() ^ 0x01);
    m_http.enqueue(corrupt);
    AssistantStreamEmitter first = makeEmitter();
    m_runner.run(
        HttpRequest{}, Model{}, StreamOptions{}, first, [](const AwsEventStreamMessage&) { return Result<void>{}; },
        []() { return Result<void>{}; }, [](const HttpResponse&) { return std::string(); });
    EXPECT_NE(first.message().errorMessage->find("checksum"), std::string::npos);

    HttpResponse truncated;
    truncated.status = 200;
    truncated.body = frame("a", "1").substr(0, 20);
    m_http.enqueue(truncated);
    AssistantStreamEmitter second = makeEmitter();
    m_runner.run(
        HttpRequest{}, Model{}, StreamOptions{}, second, [](const AwsEventStreamMessage&) { return Result<void>{}; },
        []() { return Result<void>{}; }, [](const HttpResponse&) { return std::string(); });
    EXPECT_NE(second.message().errorMessage->find("inside a message"), std::string::npos);
}
