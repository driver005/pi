#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "pi_plugin.h"

import std;
import pi.support.plugin_provider;
import pi.testing.fixed_clock;

/** Stands in for a plugin: the stream function runs `run` and reports through the sink like stream_emit does. */
class PluginProviderTest : public testing::Test {
protected:
    class Script {
    public:
        std::function<void(const Json& request, const AbortSignal* abort, PluginStreamTranslator& sink)> run;
        std::mutex mutex;
        Json request;
    };

    PluginProviderTest() {
        m_model.id = "m1";
        m_model.provider = "plug";
        m_model.api = "plug-api";
        m_provider = std::make_unique<PluginProvider>("plug-api", +[](void* userData, PiString request, const PiAbort* abort, PiStreamSink* sink) {
            auto* script = static_cast<Script*>(userData);
            const Json parsed = Json::parse(std::string(request.data, request.size), nullptr, false);
            {
                const std::lock_guard<std::mutex> lock(script->mutex);
                script->request = parsed;
            }
            script->run(parsed, reinterpret_cast<const AbortSignal*>(abort), *reinterpret_cast<PluginStreamTranslator*>(sink));
        }, &m_script, m_clock);
    }

    TranscriptContext context(const std::string& text = "hi") {
        TranscriptContext out;
        UserMessage user;
        TextContent block;
        block.text = text;
        user.content = std::vector<UserContentBlock>{block};
        out.messages.push_back(user);
        return out;
    }

    Model m_model;
    FixedClock m_clock;
    Script m_script;
    std::unique_ptr<PluginProvider> m_provider;
};

TEST_F(PluginProviderTest, StreamsTheEventsTheFunctionReports) {
    m_script.run = [](const Json&, const AbortSignal*, PluginStreamTranslator& sink) {
        sink.emit(R"({"type":"text_delta","delta":"hello"})");
        sink.emit(R"({"type":"done"})");
    };
    StreamOptions options;
    options.apiKey = "secret";
    options.headers = {{"x-a", "1"}, {"x-b", std::nullopt}};
    options.temperature = 0.5;
    options.reasoning = ThinkingLevel::High;
    const auto stream = m_provider->stream(m_model, context(), options);
    const auto result = stream->result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<TextContent>(result->content[0]).text, "hello");
    EXPECT_EQ(result->timestamp, m_clock.nowMs());
    const std::lock_guard<std::mutex> lock(m_script.mutex);
    EXPECT_EQ(m_script.request["model"]["id"], "m1");
    EXPECT_EQ(m_script.request["messages"][0]["role"], "user");
    EXPECT_EQ(m_script.request["options"]["apiKey"], "secret");
    EXPECT_EQ(m_script.request["options"]["headers"], Json::parse(R"({"x-a":"1"})"));
    EXPECT_EQ(m_script.request["options"]["temperature"], 0.5);
    EXPECT_EQ(m_script.request["options"]["reasoning"], "high");
}

TEST_F(PluginProviderTest, ReturningWithoutAFinalEventIsAnError) {
    m_script.run = [](const Json&, const AbortSignal*, PluginStreamTranslator& sink) { sink.emit(R"({"type":"text_delta","delta":"x"})"); };
    const auto result = m_provider->stream(m_model, context(), StreamOptions{})->result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->stopReason, StopReason::Error);
}

TEST_F(PluginProviderTest, TheRequestsSignalReachesTheFunction) {
    std::atomic<bool> started = false;
    m_script.run = [&started](const Json&, const AbortSignal* abort, PluginStreamTranslator& sink) {
        started = true;
        while (abort->aborted() == false) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        sink.emit(R"({"type":"text_delta","delta":"late"})");
    };
    StreamOptions options;
    options.signal = std::make_shared<AbortSignal>();
    const auto stream = m_provider->stream(m_model, context(), options);
    while (!started) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    options.signal->abort();
    EXPECT_EQ(stream->result()->stopReason, StopReason::Aborted);
}

TEST_F(PluginProviderTest, ShutdownCancelsAndWaitsForRunningStreamsThenRefusesNewOnes) {
    std::atomic<bool> started = false;
    std::atomic<bool> returned = false;
    m_script.run = [&](const Json&, const AbortSignal* abort, PluginStreamTranslator&) {
        started = true;
        while (abort->aborted() == false) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        returned = true;
    };
    const auto stream = m_provider->stream(m_model, context(), StreamOptions{});
    while (!started) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    m_provider->shutdown();
    EXPECT_TRUE(returned.load());
    EXPECT_EQ(stream->result()->stopReason, StopReason::Aborted);
    const auto after = m_provider->stream(m_model, context(), StreamOptions{})->result();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->stopReason, StopReason::Error);
    EXPECT_NE(after->errorMessage->find("unloaded"), std::string::npos);
}

TEST_F(PluginProviderTest, ConcurrentStreamsAreIndependent) {
    m_script.run = [](const Json& request, const AbortSignal*, PluginStreamTranslator& sink) {
        const std::string text = request["messages"][0]["content"][0]["text"].get<std::string>();
        sink.emit(Json{{"type", "text_delta"}, {"delta", text}}.dump());
        sink.emit(R"({"type":"done"})");
    };
    std::vector<std::shared_ptr<AssistantMessageStream>> streams;
    for (int i = 0; i < 8; ++i) {
        streams.push_back(m_provider->stream(m_model, context("q" + std::to_string(i)), StreamOptions{}));
    }
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(std::get<TextContent>(streams[static_cast<std::size_t>(i)]->result()->content[0]).text, "q" + std::to_string(i));
    }
    m_provider->shutdown();
}
