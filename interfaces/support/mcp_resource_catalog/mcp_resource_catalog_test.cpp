#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.base.boring_crypto;
import pi.support.mcp_resource_catalog;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;

class FakeResourceServer : public IMcpResourceServer {
public:
    explicit FakeResourceServer(std::string name)
        : m_name(std::move(name)) {}

    std::string serverName() const override {
        return m_name;
    }
    std::int64_t requestTimeoutMs() const override {
        return 1234;
    }
    Result<McpPage> resourcesPage(const std::optional<std::string>& cursor, const McpRequestOptions& options) override {
        m_lastCursor = cursor;
        m_lastTimeout = options.timeoutMs;
        if (m_fail) {
            return std::unexpected(Error{"rpc:-32000", "boom"});
        }
        McpPage page;
        page.items = m_resources;
        page.nextCursor = m_next;
        return page;
    }
    Result<McpPage> resourceTemplatesPage(const std::optional<std::string>&, const McpRequestOptions&) override {
        McpPage page;
        page.items = m_templates;
        return page;
    }
    Result<std::vector<Json>> allResources(const McpRequestOptions&) override {
        if (m_fail) {
            return std::unexpected(Error{"rpc:-32000", "boom"});
        }
        return m_resources;
    }
    Result<std::vector<Json>> allResourceTemplates(const McpRequestOptions&) override {
        return m_templates;
    }
    Result<Json> readResource(const std::string& uri, const McpRequestOptions&) override {
        m_read = uri;
        return m_contents;
    }

    std::string m_name;
    std::vector<Json> m_resources;
    std::vector<Json> m_templates;
    Json m_contents = Json{{"contents", Json::array()}};
    std::optional<std::string> m_next;
    std::optional<std::string> m_lastCursor;
    std::optional<std::int64_t> m_lastTimeout;
    std::string m_read;
    bool m_fail = false;
};

class McpResourceCatalogTest : public testing::Test {
protected:
    McpResourceCatalogTest() {
        m_files.createDirectories("/tmp");
    }

    std::shared_ptr<FakeResourceServer> server(const std::string& name) {
        auto made = std::make_shared<FakeResourceServer>(name);
        m_servers.push_back(made);
        return made;
    }

    std::string textOf(const AgentToolResult& result, std::size_t index = 0) {
        return std::get<TextContent>(result.content.at(index)).text;
    }

    FakeFileSystem m_files;
    BoringCrypto m_crypto;
    Base64Codec m_base64;
    FakeEnvironment m_environment;
    McpResultConverter m_converter{m_files, m_crypto, m_base64, m_environment};
    std::vector<std::shared_ptr<IMcpResourceServer>> m_servers;
    McpResourceCatalog m_catalog{[this] { return m_servers; }, m_converter};
    std::shared_ptr<AbortSignal> m_signal = std::make_shared<AbortSignal>();
};

TEST_F(McpResourceCatalogTest, ListsOnePageWithItsCursorAndTagsEachItemWithTheServer) {
    auto docs = server("docs");
    docs->m_resources = {Json::parse(R"({"uri":"file:///a","name":"a","_meta":{"x":1},"icons":[1]})"),
                         Json::parse(R"({"uri":"ui://app","name":"app"})"),
                         Json::parse(R"({"uri":"file:///page.html","name":"p","mimeType":"text/html;profile=mcp-app"})")};
    docs->m_next = "next-page";
    const auto result = m_catalog.listResources(Json{{"server", "docs"}, {"cursor", "c1"}}, m_signal);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(docs->m_lastCursor, std::optional<std::string>("c1"));
    EXPECT_EQ(docs->m_lastTimeout, std::optional<std::int64_t>(1234));
    EXPECT_EQ(result->structuredContent, Json::parse(R"({"server":"docs","resources":[{"server":"docs","uri":"file:///a","name":"a"}],"nextCursor":"next-page"})"));
    EXPECT_EQ(Json::parse(textOf(*result)), result->structuredContent);
    EXPECT_EQ(result->details["tool"], "list_mcp_resources");
}

TEST_F(McpResourceCatalogTest, ListsEveryServerSortedAndReportsTheOnesThatFail) {
    auto zeta = server("zeta");
    zeta->m_resources = {Json::parse(R"({"uri":"z://1","name":"z1"})")};
    auto alpha = server("alpha");
    alpha->m_resources = {Json::parse(R"({"uri":"a://1","name":"a1"})")};
    auto broken = server("broken");
    broken->m_fail = true;
    const auto result = m_catalog.listResources(Json::object(), m_signal);
    ASSERT_TRUE(result.has_value());
    const Json& payload = result->structuredContent;
    ASSERT_EQ(payload["resources"].size(), 2u);
    EXPECT_EQ(payload["resources"][0]["server"], "alpha");
    EXPECT_EQ(payload["resources"][1]["server"], "zeta");
    EXPECT_EQ(payload["errors"], Json::parse(R"([{"server":"broken","error":"boom"}])"));
}

TEST_F(McpResourceCatalogTest, ListsTemplatesUnderTheirOwnKey) {
    auto docs = server("docs");
    docs->m_templates = {Json::parse(R"({"uriTemplate":"file:///{p}","name":"file"})")};
    const auto result = m_catalog.listResourceTemplates(Json{{"server", "docs"}}, m_signal);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->structuredContent["resourceTemplates"][0]["uriTemplate"], "file:///{p}");
    EXPECT_EQ(result->details["tool"], "list_mcp_resource_templates");
}

TEST_F(McpResourceCatalogTest, RejectsBadListArguments) {
    server("docs");
    const auto unknown = m_catalog.listResources(Json{{"server", "nope"}}, m_signal);
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().message, "MCP server \"nope\" has no resources. Servers with resources: docs");
    EXPECT_FALSE(m_catalog.listResources(Json{{"cursor", "c"}}, m_signal).has_value());
    EXPECT_FALSE(m_catalog.listResources(Json{{"server", 1}}, m_signal).has_value());
    EXPECT_TRUE(m_catalog.listResources(Json{{"server", "  "}}, m_signal).has_value());
}

TEST_F(McpResourceCatalogTest, NoServersMeansAnEmptyListingAndAHelpfulError) {
    const auto listing = m_catalog.listResources(Json::object(), m_signal);
    ASSERT_TRUE(listing.has_value());
    EXPECT_EQ(listing->structuredContent, Json::parse(R"({"resources":[]})"));
    const auto unknown = m_catalog.listResources(Json{{"server", "x"}}, m_signal);
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().message, "MCP server \"x\" has no resources");
}

TEST_F(McpResourceCatalogTest, ReadsATextResource) {
    auto docs = server("docs");
    docs->m_contents = Json::parse(R"({"contents":[{"uri":"file:///a","mimeType":"text/plain","text":"hello","_meta":{"k":1}}]})");
    const auto result = m_catalog.readResource(Json{{"server", "docs"}, {"uri", "file:///a"}}, m_signal);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(docs->m_read, "file:///a");
    ASSERT_EQ(result->content.size(), 1u);
    EXPECT_EQ(textOf(*result), "hello");
    EXPECT_EQ(result->structuredContent, Json::parse(R"({"server":"docs","uri":"file:///a","contents":[{"uri":"file:///a","mimeType":"text/plain","text":"hello"}]})"));
    EXPECT_EQ(result->details["tool"], "read_mcp_resource");
}

TEST_F(McpResourceCatalogTest, SeveralContentsAreLabeledWithTheirUris) {
    auto docs = server("docs");
    docs->m_contents = Json::parse(R"({"contents":[{"uri":"d/a","text":"A"},{"uri":"d/b","text":"B"}]})");
    const auto result = m_catalog.readResource(Json{{"server", "docs"}, {"uri", "d"}}, m_signal);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->content.size(), 4u);
    EXPECT_EQ(textOf(*result, 0), "d/a:");
    EXPECT_EQ(textOf(*result, 1), "A");
    EXPECT_EQ(textOf(*result, 2), "d/b:");
}

TEST_F(McpResourceCatalogTest, AnEmptyResourceSaysSo) {
    auto docs = server("docs");
    const auto result = m_catalog.readResource(Json{{"server", "docs"}, {"uri", "file:///none"}}, m_signal);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(textOf(*result), "Resource file:///none is empty.");
}

TEST_F(McpResourceCatalogTest, ReadNeedsAServerAndAUri) {
    server("docs");
    EXPECT_FALSE(m_catalog.readResource(Json{{"uri", "x"}}, m_signal).has_value());
    EXPECT_FALSE(m_catalog.readResource(Json{{"server", "docs"}}, m_signal).has_value());
    EXPECT_FALSE(m_catalog.readResource(Json{{"server", "nope"}, {"uri", "x"}}, m_signal).has_value());
}

TEST_F(McpResourceCatalogTest, RecognizesAppResources) {
    EXPECT_TRUE(m_catalog.isAppResource(Json{{"uri", "ui://x"}}));
    EXPECT_TRUE(m_catalog.isAppResource(Json{{"uriTemplate", "ui://x/{id}"}}));
    EXPECT_TRUE(m_catalog.isAppResource(Json{{"uri", "file:///x"}, {"mimeType", "text/html; Profile = \"mcp-app\""}}));
    EXPECT_FALSE(m_catalog.isAppResource(Json{{"uri", "file:///x"}, {"mimeType", "text/html"}}));
}
