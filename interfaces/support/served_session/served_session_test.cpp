#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.thread_pool;
import pi.session.session_manager;
import pi.support.served_session;
import pi.testing.fake_model_runtime;
import pi.testing.sequential_id_generator;
import pi.testing.session_harness;
import pi.testing.test_session_handle;

class ServedSessionTest : public ::testing::Test {
protected:
    ServedSessionTest() {
        SessionManagerOptions options;
        options.cwd = "/work";
        options.persist = false;
        auto manager = std::make_unique<SessionManager>(options, m_harness.files(), m_harness.clock(), m_harness.ids());
        manager->open();
        SessionRuntimeRequest request;
        request.cwd = "/work";
        request.agentDir = "/agent";
        request.sessionManager = std::move(manager);
        Model faux;
        faux.id = "faux-1";
        faux.provider = "faux";
        faux.api = "faux";
        m_models.addModel(faux);
        m_models.setAuthenticated("faux", true);
        m_session = std::make_unique<ServedSession>(
            std::make_unique<TestSessionHandle>(std::move(request), m_harness.agents(), m_harness.provider(),
                                                m_harness.files(), m_harness.clock(), m_harness.ids(),
                                                m_harness.sleeper()),
            m_models, m_executor, m_ids);
    }

    ServiceContext context() const {
        return ServiceContext{std::make_shared<AbortSignal>()};
    }

    std::unique_ptr<IServiceAttachment> attach() {
        auto attachment = m_session->attachClient(context());
        EXPECT_TRUE(attachment);
        return attachment ? std::move(*attachment) : nullptr;
    }

    Json invoke(IServiceAttachment& attachment, const std::string& service, const std::string& member,
                const Json& args = Json::array()) {
        const Json call = {{"serviceId", service}, {"member", member}, {"args", args}};
        auto result = attachment.invokeService(call, m_publish, context());
        EXPECT_TRUE(result) << (result ? "" : result.error().message);
        return result && result->has_value() ? **result : Json();
    }

    SessionHarness m_harness{"/work"};
    FakeModelRuntime m_models;
    SequentialIdGenerator m_ids{"op"};
    IServiceEndpoint::Publisher m_publish = [](const std::string&, const Json&, const ServiceContext&) {};
    ThreadPool m_executor{4};
    std::unique_ptr<ServedSession> m_session;
};

TEST_F(ServedSessionTest, OffersTheThreeSessionServices) {
    auto attachment = attach();
    const Json catalogue = invoke(*attachment, "$chord.service", "catalogue");
    std::set<std::string> ids;
    for (const Json& entry : catalogue) {
        ids.insert(entry["serviceId"].get<std::string>());
    }
    EXPECT_EQ(ids, (std::set<std::string>{"pi.agent-controller", "pi.models", "pi.transcript"}));
}

TEST_F(ServedSessionTest, PromptsRunThroughTheController) {
    auto attachment = attach();
    m_harness.provider().enqueue(m_harness.provider().textResponse("served answer"));
    const Json response = invoke(*attachment, "pi.agent-controller", "prompt",
                                 Json::array({Json{{"message", "hi"}, {"images", nullptr}}}));
    ASSERT_EQ(response["accepted"], true);
    const Json result = invoke(*attachment, "pi.agent-controller", "waitForPrompt", Json::array({response["operationId"]}));
    EXPECT_EQ(result["text"], "served answer");
}

TEST_F(ServedSessionTest, SubscribersSeeTheTranscriptGrow) {
    auto attachment = attach();
    std::mutex mutex;
    std::vector<std::string> updates;
    const IServiceEndpoint::Publisher publish = [&](const std::string& id, const Json&, const ServiceContext&) {
        const std::lock_guard<std::mutex> lock(mutex);
        updates.push_back(id);
    };
    const Json subscribe = {{"serviceId", "$chord.service"},
                            {"member", "subscribe"},
                            {"args", Json::array({"t1", "pi.transcript", "singleton"})}};
    auto snapshot = attachment->invokeService(subscribe, publish, context());
    ASSERT_TRUE(snapshot);
    m_harness.provider().enqueue(m_harness.provider().textResponse("hello"));
    ASSERT_TRUE(m_session->runtime().session().prompt("hi", {}));
    const std::lock_guard<std::mutex> lock(mutex);
    EXPECT_FALSE(updates.empty());
    EXPECT_EQ(updates.front(), "t1");
}

TEST_F(ServedSessionTest, ClientsShareTheSession) {
    auto first = attach();
    auto second = attach();
    m_harness.provider().enqueue(m_harness.provider().textResponse("shared"));
    const Json response = invoke(*first, "pi.agent-controller", "prompt",
                                 Json::array({Json{{"message", "hi"}, {"images", nullptr}}}));
    const Json result = invoke(*second, "pi.agent-controller", "waitForPrompt", Json::array({response["operationId"]}));
    EXPECT_EQ(result["text"], "shared");
    first->release(context());
    EXPECT_TRUE(invoke(*second, "$chord.service", "catalogue").is_array());
}

TEST_F(ServedSessionTest, ClosingStopsTheSessionAndRefusesNewClients) {
    auto attachment = attach();
    ASSERT_TRUE(m_session->close(context()));
    ASSERT_TRUE(m_session->close(context()));
    EXPECT_TRUE(m_session->runtime().session().isIdle());
    EXPECT_EQ(m_session->attachClient(context()).error().code, "server_draining");
    const Json call = {{"serviceId", "pi.models"}, {"member", "getThinkingLevels"}, {"args", Json::array()}};
    EXPECT_FALSE(attachment->invokeService(call, m_publish, context()));
}
