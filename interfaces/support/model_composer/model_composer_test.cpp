#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.model_composer;

class ModelComposerTest : public testing::Test {
protected:
    Model builtin(const std::string& id) {
        Model m;
        m.id = id;
        m.name = id;
        m.api = "openai-completions";
        m.provider = "groq";
        m.baseUrl = "https://api.groq.com/openai/v1";
        m.contextWindow = 8000;
        m.maxTokens = 2000;
        m.cost.input = 1;
        m.cost.output = 2;
        return m;
    }

    ModelComposer m_composer;
};

TEST_F(ModelComposerTest, NullConfigKeepsBaseModels) {
    const auto models = m_composer.compose("groq", {builtin("a")}, Json());
    ASSERT_TRUE(models.has_value());
    ASSERT_EQ(models->size(), 1U);
    EXPECT_EQ((*models)[0].baseUrl, "https://api.groq.com/openai/v1");
}

TEST_F(ModelComposerTest, ProviderBaseUrlAndCompatApplyToAllModels) {
    const Json config = Json::parse(R"({"baseUrl":"http://proxy/v1","compat":{"supportsStore":false}})");
    const auto models = m_composer.compose("groq", {builtin("a"), builtin("b")}, config);
    ASSERT_TRUE(models.has_value());
    for (const auto& model : *models) {
        EXPECT_EQ(model.baseUrl, "http://proxy/v1");
        EXPECT_EQ(model.compat["supportsStore"], false);
    }
}

TEST_F(ModelComposerTest, CustomModelsInheritDefaultsAndApiFromProvider) {
    const Json config = Json::parse(R"({
        "baseUrl":"http://localhost:11434/v1","api":"openai-completions","apiKey":"ollama",
        "models":[{"id":"llama3","name":"Llama 3","reasoning":true,"contextWindow":32000,"input":["text","image"]}]})");
    const auto models = m_composer.compose("ollama", {}, config);
    ASSERT_TRUE(models.has_value());
    ASSERT_EQ(models->size(), 1U);
    const Model& model = (*models)[0];
    EXPECT_EQ(model.provider, "ollama");
    EXPECT_EQ(model.api, "openai-completions");
    EXPECT_EQ(model.baseUrl, "http://localhost:11434/v1");
    EXPECT_EQ(model.name, "Llama 3");
    EXPECT_TRUE(model.reasoning);
    EXPECT_EQ(model.contextWindow, 32000);
    EXPECT_EQ(model.maxTokens, 16384);
    EXPECT_EQ(model.input.size(), 2U);
}

TEST_F(ModelComposerTest, DefinitionReplacesBuiltinWithSameId) {
    const Json config = Json::parse(R"({"models":[{"id":"a","name":"Mine","contextWindow":1000}],"baseUrl":"http://x/v1"})");
    const auto models = m_composer.compose("groq", {builtin("a"), builtin("b")}, config);
    ASSERT_TRUE(models.has_value());
    ASSERT_EQ(models->size(), 2U);
    EXPECT_EQ((*models)[0].name, "Mine");
    EXPECT_EQ((*models)[0].contextWindow, 1000);
}

TEST_F(ModelComposerTest, ModelOverridesMergeFieldByField) {
    Model base = builtin("a");
    base.thinkingLevelMap = Json::parse(R"({"low":"l"})");
    base.compat = Json::parse(R"({"openRouterRouting":{"order":["x"]},"supportsStore":true})");
    const Json config = Json::parse(R"({"modelOverrides":{"a":{
        "name":"Renamed","cost":{"output":9},"thinkingLevelMap":{"high":"h"},
        "compat":{"openRouterRouting":{"only":["y"]}},"maxTokens":500}}})");
    const auto models = m_composer.compose("groq", {base}, config);
    ASSERT_TRUE(models.has_value());
    const Model& model = (*models)[0];
    EXPECT_EQ(model.name, "Renamed");
    EXPECT_EQ(model.cost.input, 1);
    EXPECT_EQ(model.cost.output, 9);
    EXPECT_EQ(model.maxTokens, 500);
    EXPECT_EQ(model.thinkingLevelMap["low"], "l");
    EXPECT_EQ(model.thinkingLevelMap["high"], "h");
    EXPECT_EQ(model.compat["openRouterRouting"]["order"][0], "x");
    EXPECT_EQ(model.compat["openRouterRouting"]["only"][0], "y");
    EXPECT_EQ(model.compat["supportsStore"], true);
}

TEST_F(ModelComposerTest, ReportsConfigurationErrors) {
    EXPECT_EQ(m_composer.compose("p", {}, Json::parse(R"({"models":[{"id":"m"}],"baseUrl":"http://x"})")).error().message,
              "Provider p, model m: no \"api\" specified. Set at provider or model level.");
    EXPECT_EQ(m_composer.compose("p", {}, Json::parse(R"({"models":[{"id":"m","api":"openai-completions"}]})")).error().message,
              "Provider p: \"baseUrl\" is required when defining custom models.");
    EXPECT_EQ(m_composer.compose("p", {}, Json::parse(R"({"models":[{"id":"m","api":"a","contextWindow":0}],"baseUrl":"u"})")).error().message,
              "Provider p, model m: invalid contextWindow");
    EXPECT_EQ(m_composer.compose("p", {}, Json::parse(R"({})")).error().message,
              "Provider p: must specify \"baseUrl\", \"headers\", \"compat\", \"modelOverrides\", or \"models\".");
    EXPECT_EQ(m_composer.compose("p", {}, Json::parse(R"({"oauth":"radius"})")).error().message,
              "Provider p: \"baseUrl\" is required when \"oauth\" is set.");
}
