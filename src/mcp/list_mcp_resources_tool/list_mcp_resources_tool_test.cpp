#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.base.boring_crypto;
import pi.mcp.list_mcp_resources_tool;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;

class FakeServer : public IMcpResourceServer {
public:
    std::string serverName() const override {
        return "docs";
    }
    std::int64_t requestTimeoutMs() const override {
        return 1000;
    }
    Result<McpPage> resourcesPage(const std::optional<std::string>&, const McpRequestOptions&) override {
        return McpPage{};
    }
    Result<McpPage> resourceTemplatesPage(const std::optional<std::string>&, const McpRequestOptions&) override {
        return McpPage{};
    }
    Result<std::vector<Json>> allResources(const McpRequestOptions&) override {
        return std::vector<Json>{};
    }
    Result<std::vector<Json>> allResourceTemplates(const McpRequestOptions&) override {
        return std::vector<Json>{};
    }
    Result<Json> readResource(const std::string& uri, const McpRequestOptions&) override {
        return Json{{"contents", Json::array({Json{{"uri", uri}, {"text", "body"}}})}};
    }
};

class ListMcpResourcesToolTest : public testing::Test {
protected:
    ListMcpResourcesToolTest() {
        m_files.createDirectories("/tmp");
    }

    FakeFileSystem m_files;
    BoringCrypto m_crypto;
    Base64Codec m_base64;
    FakeEnvironment m_environment;
    McpResultConverter m_converter{m_files, m_crypto, m_base64, m_environment};
    std::vector<std::shared_ptr<IMcpResourceServer>> m_servers;
    std::shared_ptr<McpResourceCatalog> m_catalog = std::make_shared<McpResourceCatalog>([this] { return m_servers; }, m_converter);
};

TEST_F(ListMcpResourcesToolTest, DeclaresItsNameAndArguments) {
    ListMcpResourcesTool tool(m_catalog);
    EXPECT_EQ(tool.definition().name, "list_mcp_resources");
    EXPECT_FALSE(tool.definition().description.empty());
    EXPECT_EQ(tool.definition().parameters["type"], "object");
    EXPECT_FALSE(tool.executionMode().has_value());
    EXPECT_TRUE(tool.promptSnippet().empty());
}

TEST_F(ListMcpResourcesToolTest, ExecutesThroughTheCatalog) {
    ListMcpResourcesTool tool(m_catalog);
    const auto result = tool.execute("call-1", Json::object(), std::make_shared<AbortSignal>(), nullptr);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->details["tool"], "list_mcp_resources");
    EXPECT_TRUE(result->structuredContent.is_object());
}
