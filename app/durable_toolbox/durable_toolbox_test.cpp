#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.durable_toolbox;

class DurableToolboxTest : public testing::Test {
protected:
    DurableToolboxTest() {
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/durable_toolbox_" + testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(m_dir);
        m_cwd = m_dir + "/project";
        std::filesystem::create_directories(m_cwd);
        m_services = std::make_unique<CodingServices>(m_dir + "/agent", m_dir + "/agent/catalog", true);
        PlatformServices& platform = m_services->platform();
        m_settings = std::make_unique<SettingsManager>(m_dir + "/agent/settings.json", m_cwd + "/.pi/settings.json", false, platform.files(), platform.locks(), platform.ids());
    }

    std::unique_ptr<DurableToolbox> open(const CodingStartupOptions& startup) {
        return std::make_unique<DurableToolbox>(*m_services, *m_settings, m_cwd, m_dir + "/agent", startup);
    }

    static std::vector<std::string> names(const DurableToolbox& toolbox) {
        std::vector<std::string> out;
        for (const std::shared_ptr<ITool>& tool : toolbox.tools()) {
            out.push_back(tool->definition().name);
        }
        return out;
    }

    void writeMcpConfig() {
        const std::string script = R"sh(
while IFS= read -r line; do
  id=${line#*\"id\":}; id=${id%%,*}
  case "$line" in
    *'"method":"initialize"'*) printf '{"jsonrpc":"2.0","id":%s,"result":{"protocolVersion":"2025-11-25","capabilities":{"tools":{}},"serverInfo":{"name":"sh","version":"1"}}}\n' "$id";;
    *'"method":"tools/list"'*) printf '{"jsonrpc":"2.0","id":%s,"result":{"tools":[{"name":"echo","description":"Echo","inputSchema":{"type":"object"}}]}}\n' "$id";;
  esac
done
)sh";
        std::filesystem::create_directories(m_dir + "/agent");
        const Json config{{"mcpServers", Json{{"sh", Json{{"command", "/bin/sh"}, {"args", Json::array({"-c", script})}}}}}};
        std::ofstream(m_dir + "/agent/mcp.json") << config.dump();
    }

    std::string m_dir;
    std::string m_cwd;
    std::unique_ptr<CodingServices> m_services;
    std::unique_ptr<SettingsManager> m_settings;
};

TEST_F(DurableToolboxTest, PluginsFromTheCommandLineContributeTools) {
    CodingStartupOptions startup;
    startup.pluginPaths = {"plugins/hello_tool/libhello_tool.so"};
    startup.noMcp = true;
    const auto toolbox = open(startup);
    EXPECT_EQ(names(*toolbox), std::vector<std::string>{"hello"});
    EXPECT_TRUE(toolbox->diagnostics().empty());
    CodingStartupOptions disabled = startup;
    disabled.noPlugins = true;
    EXPECT_TRUE(names(*open(disabled)).empty());
}

TEST_F(DurableToolboxTest, ProjectPluginsLoadOnlyWhenTheProjectIsTrusted) {
    std::filesystem::create_directories(m_cwd + "/.pi/plugins");
    std::filesystem::copy_file("plugins/hello_tool/libhello_tool.so", m_cwd + "/.pi/plugins/libhello_tool.so");
    CodingStartupOptions startup;
    startup.noMcp = true;
    EXPECT_TRUE(names(*open(startup)).empty());
    ASSERT_TRUE(m_settings->setProjectTrusted(true).has_value());
    EXPECT_EQ(names(*open(startup)), std::vector<std::string>{"hello"});
}

TEST_F(DurableToolboxTest, BrokenPluginsAreDiagnostics) {
    CodingStartupOptions startup;
    startup.noMcp = true;
    startup.pluginPaths = {"/definitely/not/a/plugin.so"};
    const auto toolbox = open(startup);
    ASSERT_EQ(toolbox->diagnostics().size(), 1u);
    EXPECT_NE(toolbox->diagnostics()[0].find("Plugin: /definitely/not/a/plugin.so"), std::string::npos);
    EXPECT_TRUE(toolbox->tools().empty());
}

TEST_F(DurableToolboxTest, McpServersFromMcpJsonContributeTools) {
    writeMcpConfig();
    CodingStartupOptions startup;
    startup.noPlugins = true;
    const auto toolbox = open(startup);
    EXPECT_EQ(names(*toolbox), std::vector<std::string>{"mcp__sh__echo"});
    EXPECT_TRUE(toolbox->diagnostics().empty());
    toolbox->onChange([] {});
    CodingStartupOptions off = startup;
    off.noMcp = true;
    EXPECT_TRUE(names(*open(off)).empty());
    CodingStartupOptions restricted = startup;
    restricted.tools = std::vector<std::string>{"read"};
    EXPECT_TRUE(names(*open(restricted)).empty());
}

TEST_F(DurableToolboxTest, BadMcpConfigAndFailingServersAreDiagnostics) {
    std::filesystem::create_directories(m_dir + "/agent");
    const Json config{{"mcpServers", Json{{"broken", Json{{"command", "/definitely/not/here"}}}, {"bad name", Json{{"command", "x"}}}}}};
    std::ofstream(m_dir + "/agent/mcp.json") << config.dump();
    CodingStartupOptions startup;
    startup.noPlugins = true;
    const auto toolbox = open(startup);
    const std::vector<std::string> diagnostics = toolbox->diagnostics();
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_NE(diagnostics[0].find("invalid server name"), std::string::npos);
    EXPECT_NE(diagnostics[1].find("MCP server \"broken\""), std::string::npos);
    EXPECT_TRUE(toolbox->tools().empty());
}

TEST_F(DurableToolboxTest, ReloadLoadsThePluginsAgainAndTellsTheListener) {
    CodingStartupOptions startup;
    startup.pluginPaths = {"plugins/hello_tool/libhello_tool.so"};
    startup.noMcp = true;
    const auto toolbox = open(startup);
    std::atomic<int> changes{0};
    toolbox->onChange([&changes] { ++changes; });
    const std::shared_ptr<ITool> before = toolbox->tools().at(0);
    ASSERT_TRUE(toolbox->reload().has_value());
    EXPECT_EQ(names(*toolbox), std::vector<std::string>{"hello"});
    EXPECT_NE(toolbox->tools().at(0), before);
    EXPECT_EQ(changes.load(), 1);
    EXPECT_TRUE(toolbox->hooks()->hasHandlers("tool_call"));
    EXPECT_TRUE(toolbox->diagnostics().empty());
}

TEST_F(DurableToolboxTest, ReloadReportsAPluginThatNoLongerLoadsAndKeepsTheOthers) {
    CodingStartupOptions startup;
    startup.pluginPaths = {"plugins/hello_tool/libhello_tool.so"};
    startup.noMcp = true;
    const auto toolbox = open(startup);
    const std::string broken = m_dir + "/agent/plugins/broken.so";
    std::filesystem::create_directories(m_dir + "/agent/plugins");
    std::ofstream(broken) << "not a shared object";
    const auto reloaded = toolbox->reload();
    ASSERT_FALSE(reloaded.has_value());
    EXPECT_EQ(reloaded.error().code, "plugin");
    EXPECT_EQ(names(*toolbox), std::vector<std::string>{"hello"});
    EXPECT_EQ(toolbox->diagnostics().size(), 1u);
    std::filesystem::remove(broken);
    ASSERT_TRUE(toolbox->reload().has_value());
    EXPECT_TRUE(toolbox->diagnostics().empty());
}

TEST_F(DurableToolboxTest, RepeatedReloadsDoNotStackTools) {
    CodingStartupOptions startup;
    startup.pluginPaths = {"plugins/hello_tool/libhello_tool.so"};
    startup.noMcp = true;
    const auto toolbox = open(startup);
    ASSERT_EQ(names(*toolbox).size(), 1u);
    ASSERT_TRUE(toolbox->reload().has_value());
    ASSERT_TRUE(toolbox->reload().has_value());
    EXPECT_EQ(names(*toolbox).size(), 1u);
}
