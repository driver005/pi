#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <cstdlib>

import std;
import pi.coding_session_handle;
import pi.ai.faux_provider;

class CodingSessionHandleTest : public testing::Test {
protected:
    CodingSessionHandleTest() {
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/coding_session_handle_" +
                testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(m_dir);
        m_cwd = m_dir + "/project";
        std::filesystem::create_directories(m_cwd);
        m_services = std::make_unique<CodingServices>(m_dir + "/agent", m_dir + "/agent/catalog", true);
    }

    std::unique_ptr<CodingSessionHandle> open(const CodingStartupOptions& options) {
        SessionRuntimeRequest request;
        request.cwd = m_cwd;
        request.agentDir = m_dir + "/agent";
        auto manager = m_services->sessions().inMemory(m_cwd);
        request.sessionManager = std::move(*manager);
        return std::make_unique<CodingSessionHandle>(std::move(request), *m_services, options);
    }

    void writeProjectSettings(const std::string& json) {
        std::filesystem::create_directories(m_cwd + "/.pi");
        std::ofstream(m_cwd + "/.pi/settings.json") << json;
    }

    std::string m_dir;
    std::string m_cwd;
    std::unique_ptr<CodingServices> m_services;
};

TEST_F(CodingSessionHandleTest, StartsWithTheFauxModelAndDefaultTools) {
    const auto handle = open({});
    EXPECT_EQ(handle->session().model().id, "faux-1");
    EXPECT_EQ(handle->session().activeToolNames(), (std::vector<std::string>{"read", "bash", "edit", "write"}));
    EXPECT_EQ(handle->session().allTools().size(), 7U);
    EXPECT_TRUE(handle->diagnostics().empty());
}

TEST_F(CodingSessionHandleTest, PromptRunsThroughTheFauxProvider) {
    m_services->models().faux()->enqueue(m_services->models().faux()->textResponse("hello there"));
    const auto handle = open({});
    ASSERT_TRUE(handle->session().prompt("hi", PromptOptions{}).has_value());
    handle->session().waitForIdle();
    EXPECT_EQ(handle->session().lastAssistantText(), "hello there");
}

TEST_F(CodingSessionHandleTest, BashToolRunsInTheProjectDirectory) {
    const auto handle = open({});
    const auto result = handle->session().executeBash("pwd", {}, false, std::nullopt);
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->output.find("project"), std::string::npos);
}

TEST_F(CodingSessionHandleTest, StartupOptionsSelectTools) {
    CodingStartupOptions options;
    options.tools = std::vector<std::string>{"ls", "grep"};
    EXPECT_EQ(open(options)->session().activeToolNames(), (std::vector<std::string>{"grep", "ls"}));
    CodingStartupOptions none;
    none.noTools = true;
    EXPECT_TRUE(open(none)->session().activeToolNames().empty());
}

TEST_F(CodingSessionHandleTest, UntrustedProjectSettingsAreIgnored) {
    writeProjectSettings(R"({"defaultTools":["ls"]})");
    EXPECT_EQ(open({})->session().activeToolNames(), (std::vector<std::string>{"read", "bash", "edit", "write"}));
    CodingStartupOptions trusted;
    trusted.trustProject = true;
    EXPECT_EQ(open(trusted)->session().activeToolNames(), (std::vector<std::string>{"ls"}));
}

TEST_F(CodingSessionHandleTest, UnknownRequestedModelIsReportedAsDiagnostic) {
    CodingStartupOptions options;
    options.model = "nope/none";
    const auto handle = open(options);
    ASSERT_EQ(handle->diagnostics().size(), 1U);
    EXPECT_EQ(handle->diagnostics()[0].type, "warning");
    EXPECT_EQ(handle->session().model().id, "faux-1");
}

TEST_F(CodingSessionHandleTest, MalformedSettingsBecomeDiagnostics) {
    std::filesystem::create_directories(m_dir + "/agent");
    std::ofstream(m_dir + "/agent/settings.json") << "{ not json";
    const auto handle = open({});
    ASSERT_FALSE(handle->diagnostics().empty());
    EXPECT_NE(handle->diagnostics()[0].message.find("Settings"), std::string::npos);
}

TEST_F(CodingSessionHandleTest, ReleasingTheSessionManagerKeepsTheTree) {
    auto handle = open({});
    const std::string id = handle->sessionManager().sessionId();
    handle->session().dispose();
    const auto manager = handle->releaseSessionManager();
    ASSERT_NE(manager, nullptr);
    EXPECT_EQ(manager->sessionId(), id);
}

TEST_F(CodingSessionHandleTest, McpServersFromMcpJsonContributeActiveTools) {
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
    const auto handle = open({});
    const auto active = handle->session().activeToolNames();
    EXPECT_NE(std::find(active.begin(), active.end(), "mcp__sh__echo"), active.end());
    EXPECT_NE(std::find(active.begin(), active.end(), "read"), active.end());
    EXPECT_TRUE(handle->diagnostics().empty());

    CodingStartupOptions off;
    off.noMcp = true;
    const auto without = open(off)->session().activeToolNames();
    EXPECT_EQ(std::find(without.begin(), without.end(), "mcp__sh__echo"), without.end());
}

TEST_F(CodingSessionHandleTest, BadMcpConfigAndFailingServersAreDiagnostics) {
    std::filesystem::create_directories(m_dir + "/agent");
    const Json config{{"mcpServers", Json{{"broken", Json{{"command", "/definitely/not/here"}}}, {"bad name", Json{{"command", "x"}}}}}};
    std::ofstream(m_dir + "/agent/mcp.json") << config.dump();
    const auto handle = open({});
    const auto diagnostics = handle->diagnostics();
    ASSERT_EQ(diagnostics.size(), 2U);
    EXPECT_NE(diagnostics[0].message.find("invalid server name"), std::string::npos);
    EXPECT_NE(diagnostics[1].message.find("MCP server \"broken\""), std::string::npos);
    EXPECT_EQ(handle->session().activeToolNames(), (std::vector<std::string>{"read", "bash", "edit", "write"}));
}

TEST_F(CodingSessionHandleTest, PluginsFromTheCommandLineContributeTools) {
    CodingStartupOptions options;
    options.pluginPaths = {"plugins/hello_tool/libhello_tool.so"};
    m_services->models().faux()->enqueue(m_services->models().faux()->toolCallResponse("hello", Json{{"name", "Ada"}}, "c1"));
    m_services->models().faux()->enqueue(m_services->models().faux()->textResponse("done"));
    const auto handle = open(options);
    const auto active = handle->session().activeToolNames();
    EXPECT_NE(std::find(active.begin(), active.end(), "hello"), active.end());
    EXPECT_TRUE(handle->diagnostics().empty());
    ASSERT_TRUE(handle->session().prompt("greet", PromptOptions{}).has_value());
    handle->session().waitForIdle();
    bool greeted = false;
    for (const AgentMessage& message : handle->session().messages()) {
        if (const auto* result = std::get_if<ToolResultMessage>(&message)) {
            greeted = std::get<TextContent>(result->content.at(0)).text == "Hello, Ada!";
        }
    }
    EXPECT_TRUE(greeted);

    CodingStartupOptions disabled = options;
    disabled.noPlugins = true;
    const auto without = open(disabled)->session().activeToolNames();
    EXPECT_EQ(std::find(without.begin(), without.end(), "hello"), without.end());
}

TEST_F(CodingSessionHandleTest, ProjectPluginsLoadOnlyWhenTheProjectIsTrusted) {
    std::filesystem::create_directories(m_cwd + "/.pi/plugins");
    std::filesystem::copy_file("plugins/hello_tool/libhello_tool.so", m_cwd + "/.pi/plugins/libhello_tool.so");
    const auto untrusted = open({})->session().activeToolNames();
    EXPECT_EQ(std::find(untrusted.begin(), untrusted.end(), "hello"), untrusted.end());
    CodingStartupOptions trusted;
    trusted.trustProject = true;
    const auto active = open(trusted)->session().activeToolNames();
    EXPECT_NE(std::find(active.begin(), active.end(), "hello"), active.end());
}

TEST_F(CodingSessionHandleTest, BrokenPluginsAreDiagnostics) {
    CodingStartupOptions options;
    options.pluginPaths = {"/definitely/not/a/plugin.so"};
    const auto diagnostics = open(options)->diagnostics();
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_NE(diagnostics[0].message.find("Plugin: /definitely/not/a/plugin.so"), std::string::npos);
}

TEST_F(CodingSessionHandleTest, APluginsVirtualModelCanBeSelectedAndRoutesToThePhysicalModel) {
    CodingStartupOptions options;
    options.pluginPaths = {"plugins/hello_router/libhello_router.so"};
    options.model = "hello-router/auto";
    m_services->models().faux()->enqueue(m_services->models().faux()->textResponse("routed answer"));
    const auto handle = open(options);
    EXPECT_TRUE(handle->diagnostics().empty());
    EXPECT_EQ(handle->session().model().provider, "hello-router");
    EXPECT_EQ(handle->session().model().id, "auto");
    ASSERT_TRUE(handle->session().prompt("hi", PromptOptions{}).has_value());
    handle->session().waitForIdle();
    EXPECT_EQ(handle->session().lastAssistantText(), "routed answer");
    const AssistantMessage* answer = nullptr;
    const std::vector<AgentMessage> messages = handle->session().messages();
    for (const AgentMessage& message : messages) {
        if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            answer = assistant;
        }
    }
    ASSERT_TRUE(answer != nullptr);
    EXPECT_EQ(answer->provider, "faux");
    EXPECT_EQ(answer->model, "faux-1");
    EXPECT_EQ(handle->session().model().id, "auto");
}
