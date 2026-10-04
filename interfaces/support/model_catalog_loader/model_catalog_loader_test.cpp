#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.model_catalog_loader;
import pi.testing.fake_file_system;

class ModelCatalogLoaderTest : public testing::Test {
protected:
    FakeFileSystem m_files;
    ModelCatalogLoader m_loader{m_files};
};

TEST_F(ModelCatalogLoaderTest, MissingDirectoryIsEmpty) {
    const auto catalog = m_loader.load("/catalog");
    ASSERT_TRUE(catalog.has_value());
    EXPECT_TRUE(catalog->empty());
    EXPECT_EQ(m_loader.generatedAtMs("/catalog"), 0);
}

TEST_F(ModelCatalogLoaderTest, ReadsFlatCatalog) {
    m_files.createDirectories("/catalog");
    m_files.writeFile("/catalog/models.json", R"({
        "anthropic":{"claude-a":{"id":"claude-a","name":"A","api":"anthropic-messages","baseUrl":"https://api.anthropic.com","provider":"anthropic","input":["text"],"cost":{"input":1,"output":2,"cacheRead":0,"cacheWrite":0},"reasoning":true,"contextWindow":100,"maxTokens":10},
                      "img":{"id":"img","type":"image"}},
        "groq":{"m":{"id":"m"}}})");
    const auto catalog = m_loader.load("/catalog");
    ASSERT_TRUE(catalog.has_value());
    ASSERT_EQ(catalog->size(), 2U);
    EXPECT_EQ((*catalog)[0].first, "anthropic");
    ASSERT_EQ((*catalog)[0].second.size(), 1U);
    EXPECT_EQ((*catalog)[0].second[0].api, "anthropic-messages");
    EXPECT_EQ((*catalog)[1].second[0].provider, "groq");
    EXPECT_GT(m_loader.generatedAtMs("/catalog"), 0);
}

TEST_F(ModelCatalogLoaderTest, ReadsPerProviderFiles) {
    m_files.createDirectories("/catalog/providers");
    m_files.writeFile("/catalog/providers/openai.json", R"({
        "openai-responses":{"chat:gpt-x":{"id":"gpt-x","type":"chat","api":"openai-responses"},"image:dall":{"id":"dall","type":"image"}},
        "openai-completions":{"chat:gpt-y":{"id":"gpt-y","api":"openai-completions"}}})");
    m_files.writeFile("/catalog/providers/notes.txt", "ignored");
    const auto catalog = m_loader.load("/catalog");
    ASSERT_TRUE(catalog.has_value());
    ASSERT_EQ(catalog->size(), 1U);
    EXPECT_EQ((*catalog)[0].first, "openai");
    EXPECT_EQ((*catalog)[0].second.size(), 2U);
}

TEST_F(ModelCatalogLoaderTest, CorruptFileIsAnError) {
    m_files.createDirectories("/catalog");
    m_files.writeFile("/catalog/models.json", "{nope");
    EXPECT_FALSE(m_loader.load("/catalog").has_value());
}

TEST_F(ModelCatalogLoaderTest, ParseModelsAcceptsArraysAndObjectsAndFixesProvider) {
    const auto fromArray = m_loader.parseModels("p", Json::parse(R"([{"id":"a","provider":"other"},{"name":"no id"}])"));
    ASSERT_EQ(fromArray.size(), 1U);
    EXPECT_EQ(fromArray[0].provider, "p");
    EXPECT_EQ(m_loader.parseModels("p", Json::parse(R"({"x":{"id":"b"}})")).size(), 1U);
}
