#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.models_service;
import pi.session.session_manager;
import pi.testing.fake_model_runtime;
import pi.testing.session_harness;
import pi.testing.test_session_handle;

class ModelsServiceTest : public ::testing::Test {
protected:
    ModelsServiceTest() {
        SessionManagerOptions options;
        options.cwd = "/work";
        options.persist = false;
        auto manager = std::make_unique<SessionManager>(options, m_harness.files(), m_harness.clock(), m_harness.ids());
        manager->open();
        SessionRuntimeRequest request;
        request.cwd = "/work";
        request.agentDir = "/agent";
        request.sessionManager = std::move(manager);
        m_handle = std::make_unique<TestSessionHandle>(std::move(request), m_harness.agents(), m_harness.provider(),
                                                       m_harness.files(), m_harness.clock(), m_harness.ids(),
                                                       m_harness.sleeper());
        Model faux;
        faux.id = "faux-1";
        faux.name = "Faux One";
        faux.provider = "faux";
        faux.api = "faux";
        m_models.addModel(faux);
        Model other;
        other.id = "other";
        other.provider = "faux";
        other.api = "faux";
        other.reasoning = true;
        m_models.addModel(other);
        Model hidden;
        hidden.id = "hidden";
        hidden.provider = "nokey";
        hidden.api = "faux";
        m_models.addModel(hidden);
        m_models.setAuthenticated("faux", true);
        m_service = std::make_unique<ModelsService>(m_handle->session(), m_models);
        m_methods = m_service->methods();
        m_state = m_service->states().at("state");
    }

    Result<std::optional<Json>> call(const std::string& method, const std::vector<Json>& args = {}) {
        return m_methods.at(method)(args, ServiceContext{std::make_shared<AbortSignal>()});
    }

    Json value() const {
        return m_state->snapshot().value;
    }

    SessionHarness m_harness{"/work"};
    FakeModelRuntime m_models;
    std::unique_ptr<TestSessionHandle> m_handle;
    std::unique_ptr<ModelsService> m_service;
    std::map<std::string, IRemoteService::Method> m_methods;
    IReplicatedState* m_state = nullptr;
};

TEST_F(ModelsServiceTest, StateStartsWithTheCatalogAndTheSessionConfiguration) {
    const Json state = value();
    EXPECT_EQ(state["catalog"]["revision"], 1);
    ASSERT_EQ(state["catalog"]["availableModels"].size(), 2u);
    EXPECT_EQ(state["catalog"]["availableModels"][0]["provider"], "faux");
    EXPECT_EQ(state["catalog"]["availableModels"][0]["modelId"], "faux-1");
    EXPECT_EQ(state["catalog"]["availableModels"][0]["name"], "Faux One");
    EXPECT_EQ(state["catalog"]["availableModels"][0]["reasoning"], false);
    EXPECT_EQ(state["catalog"]["availableModels"][1]["name"], "other");
    EXPECT_EQ(state["configuration"]["model"]["modelId"], "faux-1");
    EXPECT_EQ(state["configuration"]["thinkingLevel"], "off");
    EXPECT_EQ(state["refresh"]["status"], "idle");
    EXPECT_EQ(m_state->snapshot().sequence, 0);
}

TEST_F(ModelsServiceTest, SelectingAnUnknownModelFails) {
    EXPECT_EQ(call("select", {Json{{"provider", "faux"}, {"modelId", "nope"}}}).error().code, "model_not_found");
    EXPECT_EQ(call("select", {Json{{"provider", "faux"}}}).error().code, "invalid_request");
    EXPECT_EQ(call("select", {}).error().code, "invalid_request");
}

TEST_F(ModelsServiceTest, SelectingAModelUpdatesTheConfiguration) {
    EXPECT_FALSE(call("select", {Json{{"provider", "nokey"}, {"modelId", "hidden"}}}));
    ASSERT_TRUE(call("select", {Json{{"provider", "faux"}, {"modelId", "other"}}}));
    EXPECT_EQ(value()["configuration"]["model"]["modelId"], "other");
    EXPECT_EQ(m_handle->session().model().id, "other");
    EXPECT_GT(m_state->snapshot().sequence, 0);
}

TEST_F(ModelsServiceTest, ThinkingLevelsFollowTheModel) {
    auto levels = call("getThinkingLevels");
    ASSERT_TRUE(levels);
    EXPECT_EQ(**levels, Json::array({"off"}));
    ASSERT_TRUE(call("select", {Json{{"provider", "faux"}, {"modelId", "other"}}}));
    levels = call("getThinkingLevels");
    ASSERT_TRUE(levels);
    EXPECT_GT((*levels)->size(), 1u);
    ASSERT_TRUE(call("selectThinking", {Json("high")}));
    EXPECT_EQ(value()["configuration"]["thinkingLevel"], "high");
    EXPECT_EQ(m_handle->session().thinkingLevel(), ThinkingLevel::High);
    ASSERT_TRUE(call("cycleThinking"));
    EXPECT_NE(value()["configuration"]["thinkingLevel"], "high");
}

TEST_F(ModelsServiceTest, InvalidThinkingLevelsAreRefused) {
    EXPECT_EQ(call("selectThinking", {Json("bogus")}).error().code, "invalid_request");
    EXPECT_EQ(call("selectThinking", {Json(3)}).error().code, "invalid_request");
}

TEST_F(ModelsServiceTest, ChangesMadeThroughTheSessionReachTheState) {
    ASSERT_TRUE(call("select", {Json{{"provider", "faux"}, {"modelId", "other"}}}));
    m_handle->session().setThinkingLevel(ThinkingLevel::Low, false);
    EXPECT_EQ(value()["configuration"]["thinkingLevel"], "low");
}

TEST_F(ModelsServiceTest, RefreshBumpsTheCatalogRevision) {
    std::vector<std::string> statuses;
    m_state->subscribe([&](const Json&, std::int64_t, const ServiceContext&) { statuses.push_back(""); });
    ASSERT_TRUE(call("refresh"));
    EXPECT_EQ(value()["catalog"]["revision"], 2);
    EXPECT_EQ(value()["refresh"]["status"], "done");
    EXPECT_GE(statuses.size(), 2u);
}
