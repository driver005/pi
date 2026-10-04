#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <cstdlib>

import std;
import pi.model_services;
import pi.platform_services;

class ModelServicesTest : public testing::Test {
protected:
    ModelServicesTest() {
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/model_services_" +
                testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir);
    }

    std::string m_dir;
    PlatformServices m_platform{2};
};

TEST_F(ModelServicesTest, WithoutCatalogOrConfigThereAreNoModels) {
    ModelServices services(m_platform, m_dir, m_dir + "/catalog", false);
    EXPECT_TRUE(services.models().models().empty());
    EXPECT_EQ(services.faux(), nullptr);
}

TEST_F(ModelServicesTest, FauxModeAddsAScriptedProviderAndModel) {
    ModelServices services(m_platform, m_dir, m_dir + "/catalog", true);
    ASSERT_NE(services.faux(), nullptr);
    const auto model = services.models().find("faux", "faux-1");
    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(model->api, "faux");
    EXPECT_TRUE(services.models().hasConfiguredAuth("faux"));
    auto stream = services.models().stream(*model, TranscriptContext{}, StreamOptions{});
    while (stream->next()) {
    }
    const auto result = stream->result();
    ASSERT_TRUE(result.has_value());
    // No replies were scripted, so the faux provider reports it.
    EXPECT_EQ(result->stopReason, StopReason::Error);
}

TEST_F(ModelServicesTest, CustomModelsJsonProvidersAreLoaded) {
    std::ofstream(m_dir + "/models.json") << R"({"providers":{"local":{"baseUrl":"http://localhost:1234/v1","api":"openai-completions","apiKey":"k","models":[{"id":"m1"}]}}})";
    ModelServices services(m_platform, m_dir, m_dir + "/catalog", false);
    const auto model = services.models().find("local", "m1");
    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(model->baseUrl, "http://localhost:1234/v1");
    EXPECT_TRUE(services.models().hasConfiguredAuth("local"));
}
