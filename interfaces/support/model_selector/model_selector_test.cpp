#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.model_selector;

class ModelSelectorTest : public testing::Test {
protected:
    Model model(const std::string& provider, const std::string& id, bool reasoning) {
        Model out;
        out.provider = provider;
        out.id = id;
        out.reasoning = reasoning;
        return out;
    }

    ModelSelectionInput input() {
        ModelSelectionInput in;
        in.all = {model("a", "small", false), model("a", "big", true), model("b", "other", true)};
        in.available = {in.all[0], in.all[1]};
        return in;
    }

    ModelSelector m_selector;
};

TEST_F(ModelSelectorTest, FirstAvailableWhenNothingElseApplies) {
    const ModelChoice choice = m_selector.select(input(), SettingsView());
    ASSERT_TRUE(choice.model.has_value());
    EXPECT_EQ(choice.model->id, "small");
    EXPECT_EQ(choice.thinkingLevel, ThinkingLevel::Off);
}

TEST_F(ModelSelectorTest, SettingsDefaultsPickModelAndLevel) {
    const SettingsView settings(Json::parse(
        R"({"defaultProvider":"a","defaultModel":"big","defaultThinkingLevel":"low"})"));
    const ModelChoice choice = m_selector.select(input(), settings);
    ASSERT_TRUE(choice.model.has_value());
    EXPECT_EQ(choice.model->id, "big");
    EXPECT_EQ(choice.thinkingLevel, ThinkingLevel::Low);
}

TEST_F(ModelSelectorTest, ReasoningModelDefaultsToMedium) {
    ModelSelectionInput in = input();
    in.requestedModel = "big";
    const ModelChoice choice = m_selector.select(in, SettingsView());
    EXPECT_EQ(choice.model->id, "big");
    EXPECT_EQ(choice.thinkingLevel, ThinkingLevel::Medium);
}

TEST_F(ModelSelectorTest, RequestedModelWithoutCredentialsStillChosenWithSuffix) {
    ModelSelectionInput in = input();
    in.requestedModel = "b/other:high";
    const ModelChoice choice = m_selector.select(in, SettingsView());
    ASSERT_TRUE(choice.model.has_value());
    EXPECT_EQ(choice.model->provider, "b");
    EXPECT_EQ(choice.thinkingLevel, ThinkingLevel::High);
}

TEST_F(ModelSelectorTest, UnknownRequestedModelWarnsAndFallsBack) {
    ModelSelectionInput in = input();
    in.requestedModel = "missing";
    const ModelChoice choice = m_selector.select(in, SettingsView());
    EXPECT_EQ(choice.model->id, "small");
    ASSERT_EQ(choice.warnings.size(), 1U);
}

TEST_F(ModelSelectorTest, SavedSessionModelAndThinkingAreRestored) {
    ModelSelectionInput in = input();
    in.saved = SessionModelRef{"a", "big"};
    in.savedThinking = "high";
    const ModelChoice choice = m_selector.select(in, SettingsView());
    EXPECT_EQ(choice.model->id, "big");
    EXPECT_EQ(choice.thinkingLevel, ThinkingLevel::High);
}

TEST_F(ModelSelectorTest, UnavailableSavedModelFallsBackWithWarning) {
    ModelSelectionInput in = input();
    in.saved = SessionModelRef{"b", "other"};
    in.savedThinking = "high";
    const ModelChoice choice = m_selector.select(in, SettingsView());
    EXPECT_EQ(choice.model->id, "small");
    EXPECT_EQ(choice.warnings.size(), 1U);
}

TEST_F(ModelSelectorTest, NoModelsAvailable) {
    const ModelChoice choice = m_selector.select(ModelSelectionInput{}, SettingsView());
    EXPECT_FALSE(choice.model.has_value());
    EXPECT_EQ(choice.warnings.size(), 1U);
}
