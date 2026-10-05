#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <cstdlib>

import std;
import pi.model_services;
import pi.platform_services;
import pi.support.header_merger;
import pi.testing.fake_http_server;

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

TEST_F(ModelServicesTest, RefreshesTheRadiusCatalogFromTheGatewayAndListsItsModels) {
    FakeHttpServer gateway([](const FakeHttpRequest& request) {
        FakeHttpReply reply;
        reply.headers = {{"Content-Type", "application/json"}};
        if (request.path != "/v1/config") {
            reply.status = 404;
            return reply;
        }
        reply.chunks = {R"({"baseUrl":"http://gateway.test/v1","models":[{"id":"fresh","name":"Fresh","reasoning":false,"input":["text"],"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0},"contextWindow":1000,"maxTokens":100}]})"};
        return reply;
    });
    setenv("PI_RADIUS_GATEWAY", gateway.url("").c_str(), 1);
    setenv("RADIUS_API_KEY", "rk-test", 1);
    unsetenv("PI_OFFLINE");
    {
        ModelServices services(m_platform, m_dir, m_dir + "/catalog", false);
        EXPECT_FALSE(services.models().find("radius", "fresh").has_value());
        const auto refreshed = services.refreshRadiusCatalog();
        ASSERT_TRUE(refreshed.has_value()) << refreshed.error().message;
        const auto model = services.models().find("radius", "fresh");
        ASSERT_TRUE(model.has_value());
        EXPECT_EQ(model->api, "pi-messages");
        EXPECT_EQ(model->baseUrl, "http://gateway.test/v1");
        ASSERT_EQ(gateway.requests().size(), 1U);
        EXPECT_EQ(HeaderMerger().find(gateway.requests()[0].headers, "authorization").value_or(""), "Bearer rk-test");
    }
    {
        // A new process starts from the stored catalog, without asking the gateway.
        setenv("PI_OFFLINE", "1", 1);
        ModelServices services(m_platform, m_dir, m_dir + "/catalog", false);
        EXPECT_TRUE(services.models().find("radius", "fresh").has_value());
        EXPECT_TRUE(services.refreshRadiusCatalog().has_value());
        EXPECT_EQ(gateway.requests().size(), 1U);
    }
    unsetenv("PI_OFFLINE");
    unsetenv("RADIUS_API_KEY");
    unsetenv("PI_RADIUS_GATEWAY");
}
