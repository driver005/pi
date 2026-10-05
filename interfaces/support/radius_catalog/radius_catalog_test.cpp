#include <gtest/gtest.h>

import std;
import pi.ai.memory_models_store;
import pi.support.header_merger;
import pi.support.radius_catalog;
import pi.testing.fixed_clock;
import pi.testing.scripted_http_client;

class RadiusCatalogTest : public testing::Test {
protected:
    HttpResponse reply(int status, std::string body) {
        HttpResponse response;
        response.status = status;
        response.body = std::move(body);
        return response;
    }

    std::string header(const HttpRequest& request, const std::string& name) {
        return m_headers.find(request.headers, name).value_or("");
    }

    ScriptedHttpClient m_http;
    MemoryModelsStore m_store;
    FixedClock m_clock{1700000000000};
    RadiusCatalog m_catalog{m_http, m_store, m_clock};
    HeaderMerger m_headers;
};

TEST_F(RadiusCatalogTest, StoresTheValidModelsAsPiMessagesModelsOfTheGateway) {
    m_http.enqueue(reply(200, R"({"baseUrl":"https://radius.test/api","models":[
        {"id":"a","name":"A","reasoning":true,"input":["text","image"],"cost":{"input":1,"output":2,"cacheRead":0,"cacheWrite":0},"contextWindow":1000,"maxTokens":100,"thinkingLevelMap":{"high":"max"}},
        {"id":"missing-cost","name":"B","reasoning":false,"input":["text"],"contextWindow":1,"maxTokens":1},
        {"id":"bad-input","name":"C","reasoning":false,"input":"text","cost":{},"contextWindow":1,"maxTokens":1}]})"));
    ASSERT_TRUE(m_catalog.refresh("https://radius.test", "token").has_value());
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://radius.test/v1/config");
    EXPECT_EQ(header(request, "authorization"), "Bearer token");
    const auto stored = *m_store.read("radius");
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->models.size(), 1U);
    EXPECT_EQ(stored->models[0]["id"], "a");
    EXPECT_EQ(stored->models[0]["api"], "pi-messages");
    EXPECT_EQ(stored->models[0]["provider"], "radius");
    EXPECT_EQ(stored->models[0]["baseUrl"], "https://radius.test/api");
    EXPECT_EQ(stored->models[0]["thinkingLevelMap"]["high"], "max");
    EXPECT_FALSE(stored->lastModified.has_value());
    EXPECT_EQ(stored->checkedAt, 1700000000000LL);
}

TEST_F(RadiusCatalogTest, AsksWithoutCredentialsAndResolvesAgainstTheOrigin) {
    m_http.enqueue(reply(200, R"({"baseUrl":"https://x","models":[]})"));
    ASSERT_TRUE(m_catalog.refresh("https://radius.test/some/path", std::nullopt).has_value());
    EXPECT_EQ(m_http.requests()[0].url, "https://radius.test/v1/config");
    EXPECT_EQ(header(m_http.requests()[0], "authorization"), "");
}

TEST_F(RadiusCatalogTest, AFailedRequestLeavesTheStoredCatalogAlone) {
    ModelsStoreEntry before;
    before.models = Json::array({Json{{"id", "kept"}}});
    ASSERT_TRUE(m_store.write("radius", before).has_value());

    m_http.enqueue(reply(503, "  down for maintenance  "));
    const auto unavailable = m_catalog.refresh("https://radius.test", std::nullopt);
    ASSERT_FALSE(unavailable.has_value());
    EXPECT_EQ(unavailable.error().message, "Could not load Radius config from https://radius.test: 503: down for maintenance");

    m_http.enqueue(reply(200, "not json"));
    const auto invalid = m_catalog.refresh("https://radius.test", std::nullopt);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().message, "Invalid Radius config from https://radius.test");

    m_http.enqueue(reply(200, R"({"models":[]})"));
    EXPECT_FALSE(m_catalog.refresh("https://radius.test", std::nullopt).has_value());

    EXPECT_EQ((*m_store.read("radius"))->models[0]["id"], "kept");
}
