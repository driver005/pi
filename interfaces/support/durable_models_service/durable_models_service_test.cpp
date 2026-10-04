#include <gtest/gtest.h>

import std;
import pi.support.durable_models_service;
import pi.testing.durable_harness_fixture;

class DurableModelsServiceTest : public ::testing::Test {
protected:
    DurableModelsServiceTest() {
        Model reasoning;
        reasoning.id = "r";
        reasoning.provider = "faux";
        reasoning.api = "faux";
        reasoning.reasoning = true;
        m_fixture.models().addModel(reasoning);
        EXPECT_TRUE(m_fixture.open().has_value());
        auto view = m_fixture.harness().viewState(m_fixture.root()->id());
        EXPECT_TRUE(view.has_value());
        m_view = *view;
        m_service = std::make_unique<DurableModelsService>(m_fixture.root(), m_view, m_fixture.models(), [this](const Model& model) { m_selected.push_back(model.provider + "/" + model.id); });
        m_methods = m_service->methods();
    }

    Json call(const std::string& method, const std::vector<Json>& args = {}) {
        auto result = m_methods.at(method)(args, ServiceContext{std::make_shared<AbortSignal>()});
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
        return result && *result ? **result : Json(nullptr);
    }

    Json state() {
        return m_service->states().at("state")->snapshot().value;
    }

    DurableHarnessFixture m_fixture;
    std::shared_ptr<IReplicatedState> m_view;
    std::unique_ptr<DurableModelsService> m_service;
    std::map<std::string, IRemoteService::Method> m_methods;
    std::vector<std::string> m_selected;
};

TEST_F(DurableModelsServiceTest, StartsWithTheConversationsModelAndTheAvailableCatalog) {
    const Json initial = state();
    EXPECT_EQ(initial.at("configuration").at("model"), (Json{{"provider", "faux"}, {"modelId", "m"}}));
    EXPECT_EQ(initial.at("configuration").at("thinkingLevel"), "off");
    EXPECT_EQ(initial.at("refresh").at("status"), "idle");
    const Json available = initial.at("catalog").at("availableModels");
    ASSERT_EQ(available.size(), 2u);
    EXPECT_EQ(available[0].at("name"), "Faux model");
    EXPECT_EQ(available[1].at("name"), "r");
    EXPECT_TRUE(available[1].at("reasoning").get<bool>());
}

TEST_F(DurableModelsServiceTest, ThinkingLevelsFollowTheSelectedModel) {
    EXPECT_EQ(call("getThinkingLevels"), Json::array({"off"}));
    call("select", {Json{{"provider", "faux"}, {"modelId", "r"}}});
    const Json levels = call("getThinkingLevels");
    EXPECT_EQ(levels, Json::array({"off", "minimal", "low", "medium", "high"}));
    EXPECT_EQ(m_selected, std::vector<std::string>{"faux/r"});
    EXPECT_EQ(state().at("configuration").at("model").at("modelId"), "r");
}

TEST_F(DurableModelsServiceTest, SelectingAThinkingLevelAndCyclingChangeTheConversationsAgent) {
    call("select", {Json{{"provider", "faux"}, {"modelId", "r"}}});
    call("selectThinking", {Json("high")});
    EXPECT_EQ(state().at("configuration").at("thinkingLevel"), "high");
    // The change is the conversation's, so its view shows it too.
    EXPECT_EQ(m_view->snapshot().value.at("docs").at("pi.agent").at("thinkingLevel"), "high");
    call("cycleThinking");
    EXPECT_EQ(state().at("configuration").at("thinkingLevel"), "off");
    call("cycleThinking");
    EXPECT_EQ(state().at("configuration").at("thinkingLevel"), "minimal");
    auto refused = m_methods.at("selectThinking")({Json("xhigh")}, ServiceContext{});
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message.find("choose one of: off, minimal, low, medium, high"), std::string::npos);
}

TEST_F(DurableModelsServiceTest, SelectingAModelClampsTheThinkingLevelToWhatItOffers) {
    call("select", {Json{{"provider", "faux"}, {"modelId", "r"}}});
    call("selectThinking", {Json("high")});
    call("select", {Json{{"provider", "faux"}, {"modelId", "m"}}});
    EXPECT_EQ(state().at("configuration").at("thinkingLevel"), "off");
    EXPECT_EQ(m_selected, (std::vector<std::string>{"faux/r", "faux/m"}));
}

TEST_F(DurableModelsServiceTest, UnknownModelsAndMalformedRequestsAreErrors) {
    auto unknown = m_methods.at("select")({Json{{"provider", "faux"}, {"modelId", "nope"}}}, ServiceContext{});
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().code, "model_not_found");
    EXPECT_FALSE(m_methods.at("select")({Json("x")}, ServiceContext{}).has_value());
    EXPECT_FALSE(m_methods.at("selectThinking")({}, ServiceContext{}).has_value());
    EXPECT_TRUE(m_selected.empty());
}

TEST_F(DurableModelsServiceTest, RefreshRebuildsTheCatalogWithANewRevision) {
    const std::int64_t before = state().at("catalog").at("revision").get<std::int64_t>();
    call("refresh");
    EXPECT_EQ(state().at("refresh").at("status"), "done");
    EXPECT_EQ(state().at("catalog").at("revision").get<std::int64_t>(), before + 1);
}
