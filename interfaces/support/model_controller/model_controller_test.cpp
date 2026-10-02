#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.model_controller;
import pi.testing.fake_model_runtime;
import pi.testing.fake_settings_manager;
import pi.testing.recording_session_sink;
import pi.testing.session_harness;

class ModelControllerTest : public testing::Test {
protected:
    ModelControllerTest() : m_agent(m_harness.makeAgent()) {
        m_alpha = model("alpha", "a1", true);
        m_beta = model("alpha", "a2", false);
        m_gamma = model("alpha", "a3", true);
        m_other = model("beta", "b1", true);
        for (const Model& entry : {m_alpha, m_beta, m_gamma, m_other}) {
            m_models.addModel(entry);
        }
        m_models.setAuthenticated("alpha", true);
        m_agent->setModel(m_alpha);
        m_controller = std::make_unique<ModelController>(*m_agent, m_harness.session(), m_settings, m_models, m_sink);
    }

    Model model(const std::string& provider, const std::string& id, bool reasoning) {
        Model out;
        out.provider = provider;
        out.id = id;
        out.reasoning = reasoning;
        return out;
    }

    std::vector<SessionEntry> entriesOfType(const std::string& type) {
        std::vector<SessionEntry> out;
        for (const auto& entry : m_harness.session().entries()) {
            if (entry.type == type) {
                out.push_back(entry);
            }
        }
        return out;
    }

    SessionHarness m_harness;
    std::unique_ptr<IAgent> m_agent;
    FakeSettingsManager m_settings;
    FakeModelRuntime m_models;
    RecordingSessionSink m_sink;
    Model m_alpha;
    Model m_beta;
    Model m_gamma;
    Model m_other;
    std::unique_ptr<ModelController> m_controller;
};

TEST_F(ModelControllerTest, SetModelRecordsChangeAndPersistsDefaults) {
    ASSERT_TRUE(m_controller->setModel(m_gamma, true).has_value());
    EXPECT_EQ(m_agent->model().id, "a3");
    const auto changes = entriesOfType("model_change");
    ASSERT_EQ(changes.size(), 1U);
    EXPECT_EQ(changes[0].body.value("modelId", ""), "a3");
    const Json settings = m_settings.settings();
    EXPECT_EQ(settings["defaultProvider"], "alpha");
    EXPECT_EQ(settings["defaultModel"], "a3");
}

TEST_F(ModelControllerTest, SetModelWithoutCredentialsFails) {
    const auto result = m_controller->setModel(m_other, false);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "No API key for beta/b1");
    EXPECT_EQ(m_agent->model().id, "a1");
    EXPECT_TRUE(entriesOfType("model_change").empty());
}

TEST_F(ModelControllerTest, ThinkingLevelIsClampedAndRecordedOnlyWhenChanged) {
    m_controller->setThinkingLevel(ThinkingLevel::High, false);
    EXPECT_EQ(m_agent->thinkingLevel(), ThinkingLevel::High);
    EXPECT_EQ(entriesOfType("thinking_level_change").size(), 1U);
    ASSERT_EQ(m_sink.eventsOf(SessionEventType::ThinkingLevelChanged).size(), 1U);
    m_controller->setThinkingLevel(ThinkingLevel::High, false);
    EXPECT_EQ(entriesOfType("thinking_level_change").size(), 1U);
    // xhigh is not offered without a thinkingLevelMap entry: clamps to the nearest supported level.
    m_controller->setThinkingLevel(ThinkingLevel::XHigh, true);
    EXPECT_EQ(m_agent->thinkingLevel(), ThinkingLevel::High);
    EXPECT_EQ(m_settings.settings()["defaultThinkingLevel"], "xhigh");
}

TEST_F(ModelControllerTest, SwitchingToNonReasoningModelTurnsThinkingOff) {
    m_controller->setThinkingLevel(ThinkingLevel::Medium, false);
    ASSERT_TRUE(m_controller->setModel(m_beta, false).has_value());
    EXPECT_EQ(m_agent->thinkingLevel(), ThinkingLevel::Off);
}

TEST_F(ModelControllerTest, PerModelThinkingDefaultAppliesOnSwitch) {
    m_settings.setGlobal("modelThinkingLevels", Json::object());
    m_settings.setGlobalNested("modelThinkingLevels", "alpha/a3", "low");
    ASSERT_TRUE(m_controller->setModel(m_gamma, false).has_value());
    EXPECT_EQ(m_agent->thinkingLevel(), ThinkingLevel::Low);
}

TEST_F(ModelControllerTest, CycleThinkingLevelWalksSupportedLevels) {
    EXPECT_EQ(m_controller->cycleThinkingLevel(false), ThinkingLevel::Minimal);
    EXPECT_EQ(m_controller->cycleThinkingLevel(false), ThinkingLevel::Low);
    ASSERT_TRUE(m_controller->setModel(m_beta, false).has_value());
    EXPECT_FALSE(m_controller->cycleThinkingLevel(false).has_value());
}

TEST_F(ModelControllerTest, CycleAvailableModelsOnlyUsesAuthenticatedOnes) {
    const auto forward = m_controller->cycleModel(true, false);
    ASSERT_TRUE(forward.has_value());
    EXPECT_EQ(forward->model.id, "a2");
    EXPECT_FALSE(forward->isScoped);
    EXPECT_EQ(m_controller->cycleModel(true, false)->model.id, "a3");
    EXPECT_EQ(m_controller->cycleModel(true, false)->model.id, "a1");
    EXPECT_EQ(m_controller->cycleModel(false, false)->model.id, "a3");
}

TEST_F(ModelControllerTest, CycleScopedModelsUsesTheirThinkingLevels) {
    m_controller->setScopedModels({ScopedModel{m_alpha, std::nullopt}, ScopedModel{m_gamma, ThinkingLevel::Low},
                                   ScopedModel{m_other, std::nullopt}});
    const auto next = m_controller->cycleModel(true, false);
    ASSERT_TRUE(next.has_value());
    EXPECT_TRUE(next->isScoped);
    EXPECT_EQ(next->model.id, "a3");
    EXPECT_EQ(next->thinkingLevel, ThinkingLevel::Low);
    // The unauthenticated scoped model is skipped.
    EXPECT_EQ(m_controller->cycleModel(true, false)->model.id, "a1");
}

TEST_F(ModelControllerTest, NothingToCycleWithOneModel) {
    FakeModelRuntime single;
    single.addModel(m_alpha);
    single.setAuthenticated("alpha", true);
    ModelController controller(*m_agent, m_harness.session(), m_settings, single, m_sink);
    EXPECT_FALSE(controller.cycleModel(true, false).has_value());
}

TEST_F(ModelControllerTest, PersistingAddsModelToANonEmptyScopeAndEnabledList) {
    m_controller->setScopedModels({ScopedModel{m_alpha, std::nullopt}, ScopedModel{m_gamma, std::nullopt}});
    m_settings.setGlobal("enabledModels", Json::array({"alpha/a1", "alpha/a3"}));
    ASSERT_TRUE(m_controller->setModel(m_beta, true).has_value());
    EXPECT_EQ(m_controller->scopedModels().size(), 3U);
    EXPECT_EQ(m_settings.settings()["enabledModels"], Json::array({"alpha/a1", "alpha/a3", "alpha/a2"}));
}
