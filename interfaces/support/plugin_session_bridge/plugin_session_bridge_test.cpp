#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.boring_crypto;
import pi.base.posix_file_system;
import pi.base.posix_process_runner;
import pi.base.system_environment;
import pi.session.agent_session;
import pi.support.plugin_session_bridge;
import pi.testing.fake_model_runtime;
import pi.testing.fake_resource_loader;
import pi.testing.fake_settings_manager;
import pi.testing.scripted_tool;
import pi.testing.session_harness;
import pi.tools.tool_registry;

class PluginSessionBridgeTest : public testing::Test {
protected:
    PluginSessionBridgeTest() : m_harness("/tmp") {
        m_model.id = "faux-1";
        m_model.provider = "faux";
        m_model.api = "faux";
        m_model.contextWindow = 100000;
        m_model.maxTokens = 8000;
        m_model.reasoning = true;
        m_models.addModel(m_model);
        m_models.setAuthenticated("faux", true);
        m_models.setStreamHandler([this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
            return m_harness.provider().stream(model, context, options);
        });
        m_tools.add(std::make_shared<ScriptedTool>("read", "contents"));
        const AgentSessionConfig config{m_harness.agents(), m_harness.session(), m_settings, m_models, m_resources,
                                        m_tools, m_bash, m_harness.files(), m_harness.clock(), m_harness.ids(),
                                        m_harness.sleeper(), m_model, ThinkingLevel::Off, "/tmp", std::nullopt,
                                        std::nullopt, {}, {}};
        m_session = std::make_unique<AgentSession>(config);
        m_bridge = std::make_unique<PluginSessionBridge>(
            *m_session, m_harness.session(), m_settings, m_models,
            [] { return Json::array({Json{{"name", "docs"}}}); }, [this] { m_shutdowns += 1; });
    }

    Json call(const std::string& method, const Json& params = Json::object()) {
        return m_bridge->call(method, params, nullptr);
    }

    SessionHarness m_harness;
    FakeSettingsManager m_settings{Json{{"theme", "dark"}}};
    FakeModelRuntime m_models;
    FakeResourceLoader m_resources;
    ToolRegistry m_tools;
    PosixFileSystem m_files;
    PosixProcessRunner m_runner;
    BoringCrypto m_crypto;
    SystemEnvironment m_environment;
    BashCommandExecutor m_bash{m_runner, m_files, m_crypto, m_environment};
    Model m_model;
    std::unique_ptr<AgentSession> m_session;
    std::unique_ptr<PluginSessionBridge> m_bridge;
    int m_shutdowns = 0;
};

TEST_F(PluginSessionBridgeTest, appendsEntriesAndReadsThemBack) {
    const Json appended = call("appendEntry", Json{{"customType", "note"}, {"data", Json{{"n", 1}}}});
    ASSERT_TRUE(appended.contains("id"));
    const std::string id = appended["id"].get<std::string>();

    const Json entry = call("sessionManager.getEntry", Json{{"id", id}});
    EXPECT_EQ(entry["customType"], "note");
    EXPECT_EQ(call("sessionManager.getLeafId")["id"], id);
    EXPECT_EQ(call("sessionManager.getEntries").size(), 1U);
    EXPECT_EQ(call("sessionManager.getBranch").size(), 1U);
    EXPECT_EQ(call("sessionManager.getCwd")["cwd"], "/tmp");
    EXPECT_FALSE(call("sessionManager.getSessionId")["id"].get<std::string>().empty());
}

TEST_F(PluginSessionBridgeTest, rejectsMalformedParameters) {
    EXPECT_TRUE(call("appendEntry", Json::object()).contains("error"));
    EXPECT_TRUE(call("setLabel", Json::object()).contains("error"));
    EXPECT_TRUE(call("sendMessage", Json{{"customType", "x"}}).contains("error"));
    EXPECT_TRUE(call("sendUserMessage", Json::object()).contains("error"));
    EXPECT_TRUE(call("setThinkingLevel", Json{{"level", "enormous"}}).contains("error"));
    EXPECT_TRUE(call("setModel", Json{{"provider", "faux"}}).contains("error"));
    EXPECT_TRUE(call("setActiveTools", Json::object()).contains("error"));
    EXPECT_TRUE(call("navigateTree", Json::object()).contains("error"));
    EXPECT_TRUE(call("sessionManager.nope").contains("error"));
    EXPECT_TRUE(call("nope").contains("error"));
}

TEST_F(PluginSessionBridgeTest, labelsAnEntry) {
    const std::string id = call("appendEntry", Json{{"customType", "note"}})["id"].get<std::string>();
    EXPECT_TRUE(call("setLabel", Json{{"entryId", id}, {"label", "checkpoint"}}).contains("id"));
}

TEST_F(PluginSessionBridgeTest, sendsCustomMessagesIntoTheTranscript) {
    const Json sent = call("sendMessage", Json{{"customType", "status"}, {"content", "hello"}, {"display", true}});
    EXPECT_EQ(sent["ok"], true);
    bool found = false;
    for (const SessionEntry& entry : m_harness.session().entries()) {
        found = found || entry.type == "custom_message";
    }
    EXPECT_TRUE(found);
}

TEST_F(PluginSessionBridgeTest, sendsUserMessagesAsPrompts) {
    m_harness.provider().enqueue(m_harness.provider().textResponse("pong"));
    EXPECT_EQ(call("sendUserMessage", Json{{"text", "ping"}})["ok"], true);
    EXPECT_EQ(call("waitForIdle")["ok"], true);
    int users = 0;
    for (const SessionEntry& entry : m_harness.session().entries()) {
        users += entry.type == "message" && entry.body["message"]["role"] == "user" ? 1 : 0;
    }
    EXPECT_EQ(users, 1);
}

TEST_F(PluginSessionBridgeTest, readsAndChangesModelAndThinkingLevel) {
    EXPECT_EQ(call("getModel")["id"], "faux-1");
    EXPECT_EQ(call("getThinkingLevel")["level"], "off");
    EXPECT_EQ(call("setThinkingLevel", Json{{"level", "high"}})["ok"], true);
    EXPECT_EQ(call("getThinkingLevel")["level"], "high");
    EXPECT_EQ(call("setModel", Json{{"provider", "faux"}, {"id", "missing"}})["ok"], false);
    EXPECT_EQ(call("setModel", Json{{"provider", "faux"}, {"id", "faux-1"}})["ok"], true);
}

TEST_F(PluginSessionBridgeTest, managesSessionStateAndTools) {
    EXPECT_EQ(call("setSessionName", Json{{"name", "demo"}})["ok"], true);
    EXPECT_EQ(call("getSessionName")["name"], "demo");
    EXPECT_EQ(call("isIdle")["idle"], true);
    EXPECT_EQ(call("hasPendingMessages")["pending"], false);
    EXPECT_EQ(call("getSettings")["theme"], "dark");
    EXPECT_EQ(call("getMcpServers")[0]["name"], "docs");
    EXPECT_TRUE(call("getSystemPrompt").contains("systemPrompt"));
    ASSERT_EQ(call("getAllTools").size(), 1U);
    EXPECT_EQ(call("setActiveTools", Json{{"names", Json::array()}})["ok"], true);
    EXPECT_TRUE(call("getActiveTools").empty());
    EXPECT_TRUE(call("getCommands").is_array());
}

TEST_F(PluginSessionBridgeTest, shutdownAsksTheHost) {
    EXPECT_EQ(call("shutdown")["ok"], true);
    EXPECT_EQ(m_shutdowns, 1);
}

TEST_F(PluginSessionBridgeTest, replacingTheSessionIsNotOffered) {
    EXPECT_TRUE(call("newSession").contains("error"));
    EXPECT_TRUE(call("switchSession", Json{{"path", "/x"}}).contains("error"));
    EXPECT_TRUE(call("fork", Json{{"entryId", "e"}}).contains("error"));
}

TEST_F(PluginSessionBridgeTest, waitForIdleStopsWhenAborted) {
    AbortSignal signal;
    signal.abort();
    EXPECT_EQ(m_bridge->call("waitForIdle", Json::object(), &signal)["ok"], true);
}
