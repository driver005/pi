#include <gtest/gtest.h>

import std;
import pi.ai.faux_provider;
import pi.support.model_requests;
import pi.testing.fake_model_runtime;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;

class ModelRequestsTest : public ::testing::Test {
protected:
    ModelRequestsTest() : m_faux(m_executor, m_clock) {
        Model model;
        model.id = "m";
        model.provider = "faux";
        model.api = "faux";
        m_runtime.addModel(model);
        m_runtime.setAuthenticated("faux", true);
        m_runtime.setStreamHandler([this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
            m_lastOptions = options;
            m_lastContext = context;
            return m_faux.stream(model, context, options);
        });
    }

    InlineExecutor m_executor;
    FixedClock m_clock;
    FauxProvider m_faux;
    FakeModelRuntime m_runtime;
    ModelRequests m_requests;
    StreamOptions m_lastOptions;
    TranscriptContext m_lastContext;
};

TEST_F(ModelRequestsTest, FindResolvesReferencesOrReportsNoModel) {
    auto found = m_requests.find(&m_runtime, Json::object({{"provider", "faux"}, {"modelId", "m"}}));
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->id, "m");
    auto missing = m_requests.find(&m_runtime, Json::object({{"provider", "faux"}, {"modelId", "nope"}}));
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, "no_model");
    EXPECT_EQ(missing.error().message, "Model faux/nope is not available");
    EXPECT_FALSE(m_requests.find(nullptr, Json::object({{"provider", "faux"}, {"modelId", "m"}})).has_value());
}

TEST_F(ModelRequestsTest, OptionsMapTheCuratedSettings) {
    AbortSignal signal;
    Json stream = Json::parse(R"({"transport":"sse","timeoutMs":1500,"maxRetries":2,"maxRetryDelayMs":9,"headers":{"x-a":"1"},"metadata":{"k":"v"},"cacheRetention":"short","deferred":true})");
    StreamOptions options = m_requests.options(stream, "high", signal);
    EXPECT_EQ(options.transport, std::optional<std::string>("sse"));
    EXPECT_EQ(options.timeoutMs, std::optional<std::int64_t>(1500));
    EXPECT_EQ(options.maxRetries, std::optional<int>(2));
    EXPECT_EQ(options.maxRetryDelayMs, std::optional<std::int64_t>(9));
    ASSERT_EQ(options.headers.size(), 1u);
    EXPECT_EQ(options.headers[0].first, "x-a");
    EXPECT_EQ(options.metadata.at("k"), "v");
    EXPECT_EQ(options.cacheRetention, std::optional<std::string>("short"));
    EXPECT_EQ(options.deferred, true);
    EXPECT_EQ(options.reasoning, ThinkingLevel::High);
    // The signal handle aliases the invocation's signal.
    EXPECT_FALSE(options.signal->aborted());
    signal.abort();
    EXPECT_TRUE(options.signal->aborted());
    EXPECT_EQ(m_requests.options(Json::object(), "off", signal).reasoning, ThinkingLevel::Off);
}

TEST_F(ModelRequestsTest, CompleteReturnsTheFinalMessageAsJson) {
    m_faux.enqueue(m_faux.textResponse("hello"));
    AbortSignal signal;
    Model model = *m_runtime.find("faux", "m");
    Json messages = Json::array({Json::object({{"role", "user"}, {"content", "hi"}, {"timestamp", 1}})});
    auto message = m_requests.complete(m_runtime, model, messages, m_requests.options(Json::object(), "off", signal));
    ASSERT_TRUE(message.has_value()) << message.error().message;
    EXPECT_EQ(message->at("role"), "assistant");
    EXPECT_EQ(message->at("stopReason"), "stop");
    EXPECT_EQ(message->at("content")[0].at("text"), "hello");
    ASSERT_EQ(m_lastContext.messages.size(), 1u);
}

TEST_F(ModelRequestsTest, MalformedMessagesAndMissingStreamsFail) {
    AbortSignal signal;
    Model model = *m_runtime.find("faux", "m");
    EXPECT_FALSE(m_requests.open(m_runtime, model, Json::parse(R"([{"role":"nope"}])"), m_requests.options(Json::object(), "off", signal)).has_value());
    FakeModelRuntime empty;
    empty.addModel(model);
    auto none = m_requests.open(empty, model, Json::array(), m_requests.options(Json::object(), "off", signal));
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().code, "no_model");
}
