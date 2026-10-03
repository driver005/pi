#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.base.boring_crypto;
import pi.mcp_server_manager;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;
import pi.testing.recording_sleeper;
import pi.testing.scripted_mcp_connector;
import pi.tools.tool_registry;

class McpServerManagerTest : public testing::Test {
protected:
    McpServerConfig server(const std::string& name) {
        McpServerConfig config;
        config.name = name;
        config.command = "cmd";
        return config;
    }

    Json toolList(const std::vector<std::string>& names) {
        Json tools = Json::array();
        for (const std::string& name : names) {
            tools.push_back(Json{{"name", name}, {"description", "does " + name}, {"inputSchema", Json{{"type", "object"}}}});
        }
        return Json{{"tools", tools}};
    }

    ScriptedMcpConnector::Setup serving(std::vector<std::string> names) {
        return [this, names](ScriptedMcpTransport& transport) {
            transport.answerInitialize(Json{{"tools", Json{{"listChanged", true}}}});
            transport.onRequest("tools/list", [this, names](const Json&) -> Result<Json> {
                const std::lock_guard<std::mutex> lock(m_mutex);
                return toolList(m_listed.empty() ? names : m_listed);
            });
            transport.onRequest("tools/call", [](const Json& params) -> Result<Json> {
                return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "called " + params["name"].get<std::string>()}}})}};
            });
        };
    }

    void start(const std::vector<McpServerConfig>& servers,
               std::chrono::milliseconds wait = std::chrono::seconds(5)) {
        m_manager.start(servers, "/work", wait);
    }

    std::set<std::string> toolNames() {
        std::set<std::string> names;
        for (const auto& tool : m_registry.all()) {
            names.insert(tool->definition().name);
        }
        return names;
    }

    bool waitUntil(const std::function<bool()>& condition) {
        for (int i = 0; i < 600 && !condition(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return condition();
    }

    McpServerStatus statusOf(const std::string& name) {
        for (const McpServerStatus& status : m_manager.status()) {
            if (status.name == name) {
                return status;
            }
        }
        return McpServerStatus{};
    }

    // Declared before the manager so that listeners it triggers on shutdown still find them.
    std::mutex m_mutex;
    std::vector<std::string> m_listed;
    FakeFileSystem m_files;
    BoringCrypto m_crypto;
    Base64Codec m_base64;
    FakeEnvironment m_environment;
    ToolRegistry m_registry;
    ScriptedMcpConnector m_connector;
    RecordingSleeper m_sleeper;
    McpToolNamer m_namer{m_crypto};
    McpResultConverter m_converter{m_files, m_crypto, m_base64, m_environment};
    McpServerManager m_manager{m_registry, m_connector, m_sleeper, m_namer, m_converter, "1.0"};
};

TEST_F(McpServerManagerTest, RegistersTheToolsOfConnectedServers) {
    m_connector.enqueue(serving({"search", "fetch"}));
    start({server("docs")});
    EXPECT_EQ(toolNames(), (std::set<std::string>{"mcp__docs__search", "mcp__docs__fetch"}));
    const auto status = statusOf("docs");
    EXPECT_EQ(status.state, McpServerState::Connected);
    EXPECT_EQ(status.toolCount, 2U);
    const auto tool = m_registry.find("mcp__docs__search");
    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->definition().description, "does search");
    const auto result = tool->execute("c1", Json::object(), nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<TextContent>(result->content.at(0)).text, "called search");
}

TEST_F(McpServerManagerTest, DisabledAndHiddenServersDoNotConnect) {
    McpServerConfig off = server("off");
    off.enabled = false;
    McpServerConfig hidden = server("hidden");
    hidden.exposure = McpExposure::Hidden;
    start({off, hidden});
    EXPECT_EQ(m_connector.attempts(), 0);
    EXPECT_EQ(statusOf("off").state, McpServerState::Disabled);
    EXPECT_EQ(statusOf("off").error, "disabled");
    EXPECT_EQ(statusOf("hidden").error, "all tools are hidden");
    EXPECT_TRUE(toolNames().empty());
}

TEST_F(McpServerManagerTest, ToolExposureOverridesPickTheVisibleTools) {
    McpServerConfig config = server("docs");
    config.exposure = McpExposure::Hidden;
    config.toolExposure.emplace_back("search", McpExposure::Direct);
    m_connector.enqueue(serving({"search", "fetch"}));
    start({config});
    EXPECT_EQ(toolNames(), (std::set<std::string>{"mcp__docs__search"}));
    EXPECT_EQ(statusOf("docs").toolCount, 1U);
}

TEST_F(McpServerManagerTest, ToolsWhoseNamesCollideGetHashSuffixes) {
    m_connector.enqueue(serving({"a-b", "a_b", "plain"}));
    start({server("s")});
    const auto names = toolNames();
    ASSERT_EQ(names.size(), 3U);
    EXPECT_TRUE(names.contains("mcp__s__plain"));
    EXPECT_FALSE(names.contains("mcp__s__a_b"));
    EXPECT_EQ(std::count_if(names.begin(), names.end(), [](const std::string& name) { return name.starts_with("mcp__s__a_b_"); }), 2);
}

TEST_F(McpServerManagerTest, ListChangesAddAndRemoveTools) {
    m_connector.enqueue(serving({"one", "two"}));
    start({server("docs")});
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_listed = {"two", "three"};
    }
    m_connector.transports().at(0)->push(Json{{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}});
    ASSERT_TRUE(waitUntil([&]() { return toolNames() == std::set<std::string>{"mcp__docs__two", "mcp__docs__three"}; }));
    EXPECT_EQ(statusOf("docs").toolCount, 2U);
}

TEST_F(McpServerManagerTest, FailedServersDoNotAffectTheOthers) {
    m_connector.enqueueFailure(Error{"protocol", "broken"});
    m_connector.enqueue(serving({"ok"}));
    start({server("bad"), server("good")});
    const auto bad = statusOf("bad");
    const auto good = statusOf("good");
    EXPECT_TRUE((bad.state == McpServerState::Failed && good.state == McpServerState::Connected) ||
                (bad.state == McpServerState::Connected && good.state == McpServerState::Failed));
    EXPECT_EQ(toolNames().size(), 1U);
}

TEST_F(McpServerManagerTest, SlowServersRegisterTheirToolsWhenTheyConnect) {
    std::promise<void> gate;
    std::shared_future<void> release = gate.get_future().share();
    m_connector.enqueue([this, release](ScriptedMcpTransport& transport) {
        release.wait();
        serving({"late"})(transport);
    });
    start({server("slow")}, std::chrono::milliseconds(50));
    EXPECT_EQ(statusOf("slow").state, McpServerState::Connecting);
    EXPECT_TRUE(toolNames().empty());
    gate.set_value();
    ASSERT_TRUE(waitUntil([&]() { return toolNames().contains("mcp__slow__late"); }));
}

TEST_F(McpServerManagerTest, CloseUnregistersTheTools) {
    m_connector.enqueue(serving({"search"}));
    start({server("docs")});
    ASSERT_EQ(toolNames().size(), 1U);
    m_manager.close();
    m_manager.close();
    EXPECT_TRUE(toolNames().empty());
    EXPECT_EQ(statusOf("docs").state, McpServerState::Closed);
}

TEST_F(McpServerManagerTest, ListenerHearsAboutNewToolsOnly) {
    std::mutex mutex;
    std::vector<std::vector<std::string>> heard;
    m_manager.setToolsListener([&](const std::vector<std::string>& added) {
        const std::lock_guard<std::mutex> lock(mutex);
        heard.push_back(added);
    });
    m_connector.enqueue(serving({"one"}));
    start({server("docs")});
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_listed = {"one", "two"};
    }
    m_connector.transports().at(0)->push(Json{{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}});
    ASSERT_TRUE(waitUntil([&]() {
        const std::lock_guard<std::mutex> lock(mutex);
        return heard.size() == 2;
    }));
    const std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(heard[0], (std::vector<std::string>{"mcp__docs__one"}));
    EXPECT_EQ(heard[1], (std::vector<std::string>{"mcp__docs__two"}));
}

class McpServerManagerResourcesTest : public McpServerManagerTest {
protected:
    ScriptedMcpConnector::Setup withResources(const std::string& uri) {
        return [uri](ScriptedMcpTransport& transport) {
            transport.answerInitialize(Json{{"resources", Json::object()}});
            transport.onRequest("resources/list", [uri](const Json&) -> Result<Json> {
                return Json{{"resources", Json::array({Json{{"uri", uri}, {"name", "doc"}}})}};
            });
            transport.onRequest("resources/read", [](const Json& params) -> Result<Json> {
                return Json{{"contents", Json::array({Json{{"uri", params["uri"]}, {"text", "body of " + params["uri"].get<std::string>()}}})}};
            });
        };
    }

    std::shared_ptr<ITool> tool(const std::string& name) {
        return m_registry.find(name);
    }
};

TEST_F(McpServerManagerResourcesTest, ServersWithResourcesGetTheResourceTools) {
    m_connector.enqueue(withResources("file:///a"));
    start({server("docs")});
    EXPECT_EQ(toolNames(), (std::set<std::string>{"list_mcp_resources", "list_mcp_resource_templates", "read_mcp_resource"}));
}

TEST_F(McpServerManagerResourcesTest, ServersWithoutResourcesAddNoResourceTools) {
    m_connector.enqueue(serving({"search"}));
    start({server("docs")});
    const std::set<std::string> names = toolNames();
    EXPECT_FALSE(names.contains("list_mcp_resources"));
    EXPECT_FALSE(names.contains("read_mcp_resource"));
}

TEST_F(McpServerManagerResourcesTest, TheToolsListAndReadTheResourcesOfEveryServer) {
    m_connector.enqueue(withResources("file:///a"));
    m_connector.enqueue(withResources("file:///b"));
    start({server("alpha"), server("beta")});
    const auto signal = std::make_shared<AbortSignal>();
    const auto listing = tool("list_mcp_resources")->execute("c1", Json::object(), signal, nullptr);
    ASSERT_TRUE(listing.has_value()) << listing.error().message;
    EXPECT_EQ(listing->structuredContent["resources"].size(), 2u);
    const auto read = tool("read_mcp_resource")->execute("c2", Json{{"server", "alpha"}, {"uri", "file:///a"}}, signal, nullptr);
    ASSERT_TRUE(read.has_value()) << read.error().message;
    EXPECT_EQ(std::get<TextContent>(read->content.at(0)).text, "body of file:///a");
}

TEST_F(McpServerManagerResourcesTest, HiddenServersAreNotReached) {
    McpServerConfig hidden = server("docs");
    hidden.exposure = McpExposure::Hidden;
    hidden.toolExposure.emplace_back("x", McpExposure::Direct);
    m_connector.enqueue(withResources("file:///a"));
    start({hidden});
    EXPECT_FALSE(toolNames().contains("list_mcp_resources"));
}

TEST_F(McpServerManagerResourcesTest, ClosingWithdrawsTheResourceTools) {
    m_connector.enqueue(withResources("file:///a"));
    start({server("docs")});
    ASSERT_TRUE(toolNames().contains("read_mcp_resource"));
    m_manager.close();
    EXPECT_TRUE(toolNames().empty());
}

TEST_F(McpServerManagerTest, ServersAddedLaterConnectAndRegisterTheirTools) {
    m_connector.enqueue(serving({"search"}));
    start({server("docs")});
    m_connector.enqueue(serving({"fetch"}));
    m_manager.addServers({server("web")}, "/work");
    ASSERT_TRUE(waitUntil([&]() { return toolNames().size() == 2; }));
    EXPECT_EQ(toolNames(), (std::set<std::string>{"mcp__docs__search", "mcp__web__fetch"}));
}

TEST_F(McpServerManagerTest, AddingAKnownServerChangesNothing) {
    m_connector.enqueue(serving({"search"}));
    start({server("docs")});
    m_manager.addServers({server("docs")}, "/work");
    EXPECT_EQ(m_connector.attempts(), 1);
    EXPECT_EQ(m_manager.status().size(), 1u);
}

TEST_F(McpServerManagerTest, AStoppedServerLosesItsToolsAndItsConnection) {
    // Both servers connect at once, so they offer the same tool name.
    m_connector.enqueue(serving({"search"}));
    m_connector.enqueue(serving({"search"}));
    start({server("docs"), server("web")});
    ASSERT_EQ(toolNames().size(), 2u);
    m_manager.stopServer("docs");
    EXPECT_EQ(toolNames(), (std::set<std::string>{"mcp__web__search"}));
    EXPECT_EQ(m_manager.status().size(), 1u);
    EXPECT_EQ(m_manager.status()[0].name, "web");
    m_manager.stopServer("unknown");
    EXPECT_EQ(toolNames().size(), 1u);
}

TEST_F(McpServerManagerTest, AStoppedServerCanBeAddedAgain) {
    m_connector.enqueue(serving({"search"}));
    start({server("docs")});
    m_manager.stopServer("docs");
    m_connector.enqueue(serving({"search"}));
    m_manager.addServers({server("docs")}, "/work");
    ASSERT_TRUE(waitUntil([&]() { return toolNames().size() == 1; }));
    EXPECT_EQ(toolNames(), (std::set<std::string>{"mcp__docs__search"}));
}

TEST_F(McpServerManagerResourcesTest, StoppingTheLastResourceServerWithdrawsTheResourceTools) {
    m_connector.enqueue(withResources("file:///a"));
    start({server("docs")});
    ASSERT_TRUE(toolNames().contains("read_mcp_resource"));
    m_manager.stopServer("docs");
    EXPECT_TRUE(toolNames().empty());
}
