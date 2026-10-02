#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.models_config_loader;
import pi.testing.fake_file_system;

class ModelsConfigLoaderTest : public testing::Test {
protected:
    FakeFileSystem m_files;
    ModelsConfigLoader m_loader{m_files};
};

TEST_F(ModelsConfigLoaderTest, MissingFileIsEmptyConfig) {
    const auto config = m_loader.load("/home/user/.pi/agent/models.json");
    ASSERT_TRUE(config.has_value());
    EXPECT_TRUE(config->empty());
}

TEST_F(ModelsConfigLoaderTest, ParsesCommentsAndBom) {
    m_files.createDirectories("/c");
    m_files.writeFile("/c/models.json",
                      "\xEF\xBB\xBF{\n // local\n \"providers\": {\"ollama\": {\"baseUrl\": \"http://localhost:11434/v1\",},},\n}");
    const auto config = m_loader.load("/c/models.json");
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ((*config)["ollama"]["baseUrl"], "http://localhost:11434/v1");
}

TEST_F(ModelsConfigLoaderTest, ReportsStructuralProblemsWithFile) {
    m_files.createDirectories("/c");
    m_files.writeFile("/c/a.json", "{nope");
    EXPECT_NE(m_loader.load("/c/a.json").error().message.find("Failed to parse models.json"), std::string::npos);
    m_files.writeFile("/c/b.json", "{\"providers\":[]}");
    EXPECT_NE(m_loader.load("/c/b.json").error().message.find("providers: expected an object"), std::string::npos);
    m_files.writeFile("/c/c.json", "{\"providers\":{\"x\":1}}");
    const auto bad = m_loader.load("/c/c.json");
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().message.find("providers.x"), std::string::npos);
    EXPECT_NE(bad.error().message.find("File: /c/c.json"), std::string::npos);
}
