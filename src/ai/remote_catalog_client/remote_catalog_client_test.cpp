#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.memory_models_store;
import pi.ai.remote_catalog_client;
import pi.testing.fixed_clock;
import pi.testing.scripted_http_client;

class RemoteCatalogClientTest : public testing::Test {
protected:
    HttpResponse reply(int status, std::string body = "", HttpHeaders headers = {}) {
        HttpResponse response;
        response.status = status;
        response.body = std::move(body);
        response.headers = std::move(headers);
        return response;
    }

    std::string header(const HttpRequest& request, const std::string& name) {
        return m_headers.find(request.headers, name).value_or("");
    }

    ScriptedHttpClient m_http;
    MemoryModelsStore m_store;
    FixedClock m_clock{1700000000000};
    RemoteCatalogClient m_client{m_http, m_store, m_clock, "https://pi.test/"};
    HeaderMerger m_headers;
};

TEST_F(RemoteCatalogClientTest, FetchesAndStoresCatalog) {
    m_http.enqueue(reply(200,
                         R"({"models":[{"id":"a"},{"id":"img","type":"image"},{"id":"weird","type":"video"},{"name":"noid"}]})",
                         {{"ETag", "\"v1\""}, {"Last-Modified", "Sun, 06 Nov 1994 08:49:37 GMT"}}));
    const auto changed = m_client.refresh("anthropic", false);
    ASSERT_TRUE(changed.has_value());
    EXPECT_TRUE(*changed);
    const HttpRequest request = m_http.requests()[0];
    EXPECT_EQ(request.url, "https://pi.test/api/models/providers/anthropic?types=chat,image,classifier");
    const auto stored = *m_store.read("anthropic");
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->models.size(), 2U);
    EXPECT_EQ(stored->models[0]["provider"], "anthropic");
    EXPECT_EQ(stored->etag, "\"v1\"");
    EXPECT_EQ(stored->lastModified, 784111777000LL);
    EXPECT_EQ(stored->checkedAt, 1700000000000LL);
}

TEST_F(RemoteCatalogClientTest, AcceptsArrayAndObjectBodies) {
    m_http.enqueue(reply(200, R"([{"id":"a"}])"));
    ASSERT_TRUE(m_client.refresh("p", true));
    EXPECT_EQ((*m_store.read("p"))->models.size(), 1U);
    m_http.enqueue(reply(200, R"({"x":{"id":"a"},"y":{"id":"b"}})"));
    ASSERT_TRUE(m_client.refresh("p", true));
    EXPECT_EQ((*m_store.read("p"))->models.size(), 2U);
}

TEST_F(RemoteCatalogClientTest, FreshEntryIsNotRefetchedUnlessForced) {
    ModelsStoreEntry entry;
    entry.models = Json::parse(R"([{"id":"a"}])");
    entry.checkedAt = 1700000000000 - 1000;
    entry.lastModified = 5;
    m_store.write("p", entry);
    const auto skipped = m_client.refresh("p", false);
    ASSERT_TRUE(skipped.has_value());
    EXPECT_FALSE(*skipped);
    EXPECT_EQ(m_http.calls(), 0);
    m_http.enqueue(reply(200, R"([{"id":"b"}])"));
    ASSERT_TRUE(m_client.refresh("p", true));
    EXPECT_EQ(m_http.calls(), 1);
}

TEST_F(RemoteCatalogClientTest, StaleEntryRevalidatesWithEtagAndKeepsBodyOn304) {
    ModelsStoreEntry entry;
    entry.models = Json::parse(R"([{"id":"a"}])");
    entry.checkedAt = 1700000000000 - RemoteCatalogClient::RefreshIntervalMs - 1;
    entry.lastModified = 5;
    entry.etag = "\"abc\"";
    m_store.write("p", entry);
    m_http.enqueue(reply(304));
    ASSERT_TRUE(m_client.refresh("p", false));
    EXPECT_EQ(header(m_http.requests()[0], "if-none-match"), "\"abc\"");
    const auto stored = *m_store.read("p");
    EXPECT_EQ(stored->models.size(), 1U);
    EXPECT_EQ(stored->checkedAt, 1700000000000LL);
    EXPECT_EQ(stored->etag, "\"abc\"");
}

TEST_F(RemoteCatalogClientTest, NotFoundMarksProviderAsUnserved) {
    m_http.enqueue(reply(404));
    ASSERT_TRUE(m_client.refresh("p", true));
    const auto stored = *m_store.read("p");
    EXPECT_EQ(stored->lastModified, 0);
    EXPECT_FALSE(stored->etag.has_value());
}

TEST_F(RemoteCatalogClientTest, ServerErrorKeepsCacheAndReportsFailure) {
    ModelsStoreEntry entry;
    entry.models = Json::parse(R"([{"id":"a"}])");
    entry.etag = "\"abc\"";
    entry.lastModified = 5;
    m_store.write("p", entry);
    m_http.enqueue(reply(503));
    const auto result = m_client.refresh("p", true);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Model catalog request failed for p: 503");
    const auto stored = *m_store.read("p");
    EXPECT_EQ(stored->models.size(), 1U);
    EXPECT_EQ(stored->etag, "\"abc\"");
    EXPECT_EQ(stored->checkedAt, 1700000000000LL);
}

TEST_F(RemoteCatalogClientTest, InvalidBodyIsAnError) {
    m_http.enqueue(reply(200, "not json"));
    EXPECT_FALSE(m_client.refresh("p", true).has_value());
    m_http.enqueue(reply(200, "42"));
    EXPECT_FALSE(m_client.refresh("p", true).has_value());
}

TEST_F(RemoteCatalogClientTest, ProviderIdIsUrlEncoded) {
    m_http.enqueue(reply(200, "[]"));
    ASSERT_TRUE(m_client.refresh("my provider/x", true));
    EXPECT_NE(m_http.requests()[0].url.find("/providers/my%20provider%2Fx?"), std::string::npos);
}
