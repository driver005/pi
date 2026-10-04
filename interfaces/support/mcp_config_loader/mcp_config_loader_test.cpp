#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.mcp_config_loader;
import pi.testing.fake_file_system;

class McpConfigLoaderTest : public testing::Test {
protected:
    McpConfigLoaderTest() : m_loader(m_files) {
        m_files.createDirectories("/agent");
        m_files.createDirectories("/work/.pi");
    }

    void global(const Json& config) {
        m_files.writeFile("/agent/mcp.json", config.dump());
    }

    void project(const Json& config) {
        m_files.writeFile("/work/.pi/mcp.json", config.dump());
    }

    McpConfigResult load(bool trusted) {
        McpConfigLoadOptions options;
        options.agentDir = "/agent";
        options.cwd = "/work";
        options.projectTrusted = trusted;
        return m_loader.load(options);
    }

    FakeFileSystem m_files;
    McpConfigLoader m_loader;
};

TEST_F(McpConfigLoaderTest, MissingFilesGiveNoServers) {
    const auto result = load(true);
    EXPECT_TRUE(result.servers.empty());
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(*result.projectConfig, "/work/.pi/mcp.json");
    EXPECT_FALSE(load(false).projectConfig.has_value());
}

TEST_F(McpConfigLoaderTest, ReadsGlobalServers) {
    global(Json{{"mcpServers", Json{{"fs", Json{{"command", "npx"}}},
                                    {"docs", Json{{"url", "https://example.com/mcp"}, {"enabled", false}}}}}});
    const auto result = load(false);
    ASSERT_EQ(result.servers.size(), 2U);
    EXPECT_EQ(result.servers[0].name, "fs");
    EXPECT_EQ(result.servers[0].scope, "global");
    EXPECT_EQ(result.servers[0].source, "/agent/mcp.json");
    EXPECT_TRUE(result.servers[1].http);
    EXPECT_FALSE(result.servers[1].enabled);
    EXPECT_TRUE(result.errors.empty());
}

TEST_F(McpConfigLoaderTest, ProjectFileIsIgnoredWhenUntrusted) {
    project(Json{{"mcpServers", Json{{"local", Json{{"command", "x"}}}}}});
    EXPECT_TRUE(load(false).servers.empty());
    const auto trusted = load(true);
    ASSERT_EQ(trusted.servers.size(), 1U);
    EXPECT_EQ(trusted.servers[0].scope, "project");
}

TEST_F(McpConfigLoaderTest, ProjectEntriesReplaceGlobalOnesInPlace) {
    global(Json{{"mcpServers", Json{{"a", Json{{"command", "one"}}}, {"b", Json{{"command", "two"}}}}}});
    project(Json{{"mcpServers", Json{{"a", Json{{"command", "three"}}}}}});
    const auto result = load(true);
    ASSERT_EQ(result.servers.size(), 2U);
    EXPECT_EQ(result.servers[0].command, "three");
    EXPECT_EQ(result.servers[0].scope, "project");
}

TEST_F(McpConfigLoaderTest, ProjectOverridesKeepTheGlobalDefinition) {
    global(Json{{"mcpServers", Json{{"a", Json{{"url", "https://x/mcp"}, {"headers", Json{{"Authorization", "Bearer ${T}"}}}}}}}});
    project(Json{{"mcpServers", Json{{"a", Json{{"enabled", false}, {"exposure", "direct"}}}}}});
    const auto result = load(true);
    ASSERT_EQ(result.servers.size(), 1U);
    const McpServerConfig& server = result.servers[0];
    EXPECT_FALSE(server.enabled);
    EXPECT_EQ(server.exposure, McpExposure::Direct);
    EXPECT_EQ(server.headers.at("Authorization"), "Bearer ${T}");
    EXPECT_EQ(server.scope, "global");
    EXPECT_EQ(server.source, "/agent/mcp.json");
    EXPECT_EQ(*server.overrideSource, "/work/.pi/mcp.json");
    EXPECT_TRUE(result.errors.empty());
}

TEST_F(McpConfigLoaderTest, OverrideProblemsAreReported) {
    global(Json{{"mcpServers", Json{{"a", Json{{"command", "x"}}}}}});
    project(Json{{"mcpServers", Json{{"missing", Json{{"enabled", false}}},
                                     {"a", Json{{"enabled", false}, {"args", Json::array()}}}}}});
    const auto result = load(true);
    ASSERT_EQ(result.errors.size(), 2U);
    EXPECT_EQ(result.errors[0], "/work/.pi/mcp.json: server \"missing\" needs \"command\" or \"url\", or a global server to override");
    EXPECT_EQ(result.errors[1], "/work/.pi/mcp.json: server \"a\": an override can only set enabled, exposure, toolExposure");
    EXPECT_TRUE(result.servers[0].enabled);
}

TEST_F(McpConfigLoaderTest, NamespaceClashesAreRejected) {
    global(Json{{"mcpServers", Json{{"my-server", Json{{"command", "x"}}}, {"my_server", Json{{"command", "y"}}}}}});
    const auto result = load(false);
    ASSERT_EQ(result.servers.size(), 1U);
    EXPECT_EQ(result.servers[0].name, "my-server");
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_EQ(result.errors[0], "/agent/mcp.json: server \"my_server\" conflicts with \"my-server\"");
}

TEST_F(McpConfigLoaderTest, ProjectCannotChooseTheCredentialTarget) {
    project(Json{{"mcpServers", Json{{"a", Json{{"url", "https://x/mcp"}, {"auth", Json{{"provider", "p"}}}}}}}});
    const auto result = load(true);
    EXPECT_TRUE(result.servers.empty());
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_NE(result.errors[0].find("only allowed in the global mcp.json"), std::string::npos);
}

TEST_F(McpConfigLoaderTest, BadFilesAreReportedWithTheirPath) {
    m_files.writeFile("/agent/mcp.json", "{nope");
    m_files.writeFile("/work/.pi/mcp.json", "{\"mcpServers\": []}");
    const auto result = load(true);
    ASSERT_EQ(result.errors.size(), 2U);
    EXPECT_EQ(result.errors[0], "/agent/mcp.json: invalid JSON");
    EXPECT_EQ(result.errors[1], "/work/.pi/mcp.json: expected an object with an \"mcpServers\" object");
}

TEST_F(McpConfigLoaderTest, InvalidEntriesDoNotStopTheOthers) {
    global(Json{{"mcpServers", Json{{"bad", Json{{"enabled", 1}}}, {"good", Json{{"command", "x"}}}}}});
    const auto result = load(false);
    ASSERT_EQ(result.servers.size(), 1U);
    EXPECT_EQ(result.servers[0].name, "good");
    EXPECT_EQ(result.errors.size(), 1U);
}
