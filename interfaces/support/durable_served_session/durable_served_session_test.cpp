#include <gtest/gtest.h>

import std;
import pi.ai.faux_provider;
import pi.durable.memory_storage;
import pi.support.durable_served_session;
import pi.testing.fake_model_runtime;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;

class DurableServedSessionTest : public ::testing::Test {
protected:
    DurableServedSessionTest() : m_faux(m_executor, m_clock) {
        Model model;
        model.id = "m";
        model.provider = "faux";
        model.api = "faux";
        model.contextWindow = 100000;
        model.maxTokens = 4096;
        m_models.addModel(model);
        m_models.setAuthenticated("faux", true);
        m_models.setStreamHandler([this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
            return m_faux.stream(model, context, options);
        });
        auto registry = std::make_shared<Registry>(BuiltinTasks().all());
        HarnessOptions options;
        options.models = &m_models;
        options.registry = registry.get();
        auto harness = std::make_unique<Harness>(std::make_shared<MemoryStorage>(), options);
        EXPECT_TRUE(harness->open().has_value());
        ConversationCreateOptions create;
        create.agent = Json::object({{"model", Json::object({{"provider", "faux"}, {"modelId", "m"}})}});
        auto root = harness->root(create);
        EXPECT_TRUE(root.has_value());
        m_session = std::make_unique<DurableServedSession>(std::move(registry), std::move(harness), *root, m_models, nullptr, [this] { ++m_closedHooks; });
    }

    Json call(IServiceAttachment& attachment, const std::string& service, const std::string& member, const Json& args) {
        auto result = attachment.invokeService(Json{{"serviceId", service}, {"member", member}, {"args", args}}, [](const std::string&, const Json&, const ServiceContext&) {},
                                               ServiceContext{std::make_shared<AbortSignal>()});
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
        return result && *result ? **result : Json(nullptr);
    }

    InlineExecutor m_executor;
    FixedClock m_clock;
    FauxProvider m_faux;
    FakeModelRuntime m_models;
    int m_closedHooks = 0;
    std::unique_ptr<DurableServedSession> m_session;
};

TEST_F(DurableServedSessionTest, ServesTheThreeSessionServicesToEveryClient) {
    auto first = m_session->attachClient(ServiceContext{});
    auto second = m_session->attachClient(ServiceContext{});
    ASSERT_TRUE(first.has_value() && second.has_value());
    const std::string dump = call(**first, "$chord.service", "catalogue", Json::array()).dump();
    for (const char* service : {"pi.agent-controller", "pi.models", "pi.transcript"}) {
        EXPECT_NE(dump.find(service), std::string::npos) << service << " in " << dump;
    }
    m_faux.enqueue(m_faux.textResponse("shared"));
    const Json prompted = call(**first, "pi.agent-controller", "prompt", Json::array({Json{{"message", "hi"}, {"images", nullptr}}}));
    ASSERT_TRUE(prompted.at("accepted").get<bool>());
    // The other client waits for the same operation.
    EXPECT_EQ(call(**second, "pi.agent-controller", "waitForPrompt", Json::array({prompted.at("operationId")})).at("text"), "shared");
}

TEST_F(DurableServedSessionTest, ClosingRefusesNewClientsAndIsIdempotent) {
    ASSERT_TRUE(m_session->close(ServiceContext{}).has_value());
    EXPECT_TRUE(m_session->close(ServiceContext{}).has_value());
    EXPECT_EQ(m_closedHooks, 1);
    auto refused = m_session->attachClient(ServiceContext{});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, "server_draining");
}
