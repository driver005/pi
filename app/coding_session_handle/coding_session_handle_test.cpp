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

TEST_F(CodingSessionHandleTest, PackagesContributeSkillsPromptTemplatesAndPlugins) {
    const std::string package = m_dir + "/pkg";
    std::filesystem::create_directories(package + "/skills/greet");
    std::filesystem::create_directories(package + "/prompts");
    std::filesystem::create_directories(package + "/plugins");
    std::ofstream(package + "/skills/greet/SKILL.md") << "---\nname: greet\ndescription: Greets people\n---\nSay hello.\n";
    std::ofstream(package + "/prompts/review.md") << "---\ndescription: Review code\n---\nReview $1\n";
    std::filesystem::copy_file("plugins/hello_tool/libhello_tool.so", package + "/plugins/libhello_tool.so");
    std::filesystem::create_directories(m_dir + "/agent");
    std::ofstream(m_dir + "/agent/settings.json") << R"({"packages":["../pkg"]})";
    const auto handle = open({});
    EXPECT_TRUE(handle->diagnostics().empty());
    std::vector<std::string> commands;
    for (const SlashCommandInfo& command : handle->session().slashCommands()) {
        commands.push_back(command.name);
    }
    EXPECT_NE(std::ranges::find(commands, "skill:greet"), commands.end());
    EXPECT_NE(std::ranges::find(commands, "review"), commands.end());
    const auto active = handle->session().activeToolNames();
    EXPECT_NE(std::ranges::find(active, "hello"), active.end());

    CodingStartupOptions bare;
    bare.noSkills = true;
    bare.noPromptTemplates = true;
    bare.noPlugins = true;
    const auto without = open(bare);
    const auto bareTools = without->session().activeToolNames();
    EXPECT_EQ(std::ranges::find(bareTools, "hello"), bareTools.end());
    for (const SlashCommandInfo& command : without->session().slashCommands()) {
        EXPECT_NE(command.name, "review");
        EXPECT_NE(command.name, "skill:greet");
    }
}

TEST_F(CodingSessionHandleTest, BrokenPluginsAreDiagnostics) {
    CodingStartupOptions options;
    options.pluginPaths = {"/definitely/not/a/plugin.so"};
    const auto diagnostics = open(options)->diagnostics();
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_NE(diagnostics[0].message.find("Plugin: /definitely/not/a/plugin.so"), std::string::npos);
}

TEST_F(CodingSessionHandleTest, APluginsStreamProviderAnswersPromptsAndCallsTools) {
    CodingStartupOptions options;
    options.pluginPaths = {"plugins/hello_stream/libhello_stream.so"};
    options.model = "hello-stream/echo";
    const auto handle = open(options);
    EXPECT_TRUE(handle->diagnostics().empty());
    EXPECT_EQ(handle->session().model().provider, "hello-stream");
    ASSERT_TRUE(handle->session().prompt("hi there", PromptOptions{}).has_value());
    handle->session().waitForIdle();
    EXPECT_EQ(handle->session().lastAssistantText(), "You said: hi there");

    ASSERT_TRUE(handle->session().prompt("call ls .", PromptOptions{}).has_value());
    handle->session().waitForIdle();
    bool listed = false;
    for (const AgentMessage& message : handle->session().messages()) {
        if (const auto* result = std::get_if<ToolResultMessage>(&message)) {
            listed = result->toolName == "ls";
        }
    }
    EXPECT_TRUE(listed) << "the plugin's tool call ran the ls tool";
    EXPECT_EQ(handle->session().lastAssistantText(), "The tool answered.");
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

TEST_F(CodingSessionHandleTest, APluginsCommandsRunInsteadOfReachingTheModel) {
    CodingStartupOptions options;
    options.pluginPaths = {"plugins/hello_commands/libhello_commands.so"};
    const auto handle = open(options);
    EXPECT_TRUE(handle->diagnostics().empty());

    bool listed = false;
    for (const SlashCommandInfo& command : handle->session().slashCommands()) {
        listed = listed || (command.name == "note" && command.source == "extension");
    }
    EXPECT_TRUE(listed);

    const auto handled = handle->session().prompt("/note remember this", PromptOptions{});
    ASSERT_TRUE(handled.has_value());
    EXPECT_EQ(*handled, PromptDisposition::Handled);
    EXPECT_EQ(m_services->models().faux()->callCount(), 0);
    std::vector<std::string> notes;
    for (const SessionEntry& entry : handle->sessionManager().entries()) {
        if (entry.type == "custom" && entry.body.value("customType", "") == "hello-note") {
            notes.push_back(entry.body["data"]["text"].get<std::string>());
        }
    }
    EXPECT_EQ(notes, (std::vector<std::string>{"note: remember this"}));

    const auto failed = handle->session().prompt("/note", PromptOptions{});
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "note needs text");
}

TEST_F(CodingSessionHandleTest, PluginFlagsComeFromTheCommandLine) {
    CodingStartupOptions options;
    options.pluginPaths = {"plugins/hello_commands/libhello_commands.so"};
    options.pluginFlags = {{"loud", std::nullopt}, {"note-prefix", "todo: "}, {"missing", std::nullopt}};
    const auto handle = open(options);
    ASSERT_EQ(handle->diagnostics().size(), 1U);
    EXPECT_EQ(handle->diagnostics()[0].message, "Unknown option: --missing");

    ASSERT_TRUE(handle->session().prompt("/note buy milk", PromptOptions{}).has_value());
    bool found = false;
    for (const SessionEntry& entry : handle->sessionManager().entries()) {
        found = found || (entry.type == "custom" && entry.body["data"].value("text", "") == "TODO: BUY MILK");
    }
    EXPECT_TRUE(found);
}

TEST_F(CodingSessionHandleTest, PluginsShareAnEventBus) {
    CodingStartupOptions options;
    options.pluginPaths = {"plugins/hello_commands/libhello_commands.so"};
    const auto handle = open(options);
    ASSERT_TRUE(handle->session().prompt("/ping", PromptOptions{}).has_value());
    int pings = 0;
    for (const SessionEntry& entry : handle->sessionManager().entries()) {
        pings += entry.type == "custom" && entry.body.value("customType", "") == "hello-ping" ? 1 : 0;
    }
    EXPECT_EQ(pings, 1);
}

TEST_F(CodingSessionHandleTest, APluginAnswersTheProjectTrustQuestion) {
    writeProjectSettings(R"({"defaultTools":["ls"]})");
    CodingStartupOptions options;
    options.pluginPaths = {"plugins/hello_commands/libhello_commands.so"};
    EXPECT_EQ(open(options)->session().activeToolNames(), (std::vector<std::string>{"read", "bash", "edit", "write"})) << "no answer: not trusted";
    options.pluginFlags = {{"trust-projects", std::nullopt}};
    const auto handle = open(options);
    EXPECT_TRUE(handle->diagnostics().empty());
    EXPECT_EQ(handle->session().activeToolNames(), (std::vector<std::string>{"ls"}));
}

TEST_F(CodingSessionHandleTest, PluginsAddSkillsWhenResourcesAreDiscovered) {
    std::filesystem::create_directories(m_dir + "/extra/greet");
    std::ofstream(m_dir + "/extra/greet/SKILL.md") << "---\nname: greet\ndescription: Greets people\n---\nSay hello.\n";
    CodingStartupOptions options;
    options.pluginPaths = {"plugins/hello_commands/libhello_commands.so"};
    options.pluginFlags = {{"extra-skills", m_dir + "/extra"}};
    const auto handle = open(options);
    bool listed = false;
    for (const SlashCommandInfo& command : handle->session().slashCommands()) {
        listed = listed || command.name == "skill:greet";
    }
    EXPECT_TRUE(listed);
    EXPECT_EQ(handle->session().reload().has_value(), true);
    int greets = 0;
    for (const SlashCommandInfo& command : handle->session().slashCommands()) {
        greets += command.name == "skill:greet" ? 1 : 0;
    }
    EXPECT_EQ(greets, 1) << "a reload does not add the path twice";
}

TEST_F(CodingSessionHandleTest, APluginCanCancelSwitchingSessions) {
    CodingStartupOptions options;
    options.pluginPaths = {"plugins/hello_commands/libhello_commands.so"};
    EXPECT_TRUE(open(options)->allowSwitch("new", std::nullopt));
    options.pluginFlags = {{"keep-session", std::nullopt}};
    const auto handle = open(options);
    EXPECT_FALSE(handle->allowSwitch("resume", "/s/a.jsonl"));
    EXPECT_TRUE(handle->allowFork("e1", ForkPosition::Before)) << "only switching was cancelled";
}
