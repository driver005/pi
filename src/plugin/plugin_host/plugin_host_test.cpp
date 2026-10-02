#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "pi_plugin.h"

import std;
import pi.base.posix_dynamic_libraries;
import pi.plugin.plugin_host;
import pi.support.hook_bus;
import pi.testing.fake_dynamic_libraries;
import pi.testing.scripted_process_runner;
import pi.tools.tool_registry;

// The fake plugins below are plain C functions, so what they observe lives in globals.
const PiHostApi* g_host = nullptr;
std::string g_lastLog;
std::string g_lastReply;
int g_shutdowns = 0;

class RecordingLogger : public ILogger {
public:
    void log(LogLevel level, const std::string& message) override {
        m_entries.emplace_back(level, message);
    }

    std::vector<std::pair<LogLevel, std::string>> m_entries;
};

class PluginHostTest : public testing::Test {
protected:
    PluginHostTest()
        : m_runner([this](const ProcessRequest& request) -> Result<ProcessResult> {
              m_processRequests.push_back(request);
              if (m_onRun) {
                  m_onRun(request);
              }
              ProcessResult result;
              result.exitCode = 3;
              result.output = "ran";
              return result;
          }),
          m_host(m_libraries, m_registry, m_bus, m_runner, m_logger, PluginContext{"/work", "/agent"}) {
        g_host = nullptr;
        g_lastLog.clear();
        g_lastReply.clear();
        g_shutdowns = 0;
    }

    static PiOwnedString text(const std::string& value) {
        char* copy = new char[value.size()];
        std::memcpy(copy, value.data(), value.size());
        return PiOwnedString{copy, value.size(), [](char* data, std::size_t) { delete[] data; }};
    }

    static std::string take(PiOwnedString value) {
        std::string out = value.data != nullptr ? std::string(value.data, value.size) : std::string();
        if (value.release != nullptr) {
            value.release(value.data, value.size);
        }
        return out;
    }

    static PiString view(const std::string& value) {
        return PiString{value.data(), value.size()};
    }

    /** A well-behaved plugin: one tool, one hook, shutdown counter. */
    void provideGoodPlugin(const std::string& path) {
        PiPluginAbiVersionFn version = []() { return PI_PLUGIN_ABI_VERSION; };
        PiPluginInitFn init = [](const PiHostApi* host) {
            g_host = host;
            const std::string tool = R"({"name":"echo","description":"Echoes","parameters":{"type":"object"}})";
            PiToolExecuteFn execute = [](void*, PiString, PiString params, const PiAbort*, PiUpdateFn, void*) {
                const Json reply{{"content", Json::array({Json{{"type", "text"}, {"text", std::string(params.data, params.size)}}})}};
                return text(reply.dump());
            };
            g_lastReply = take(host->register_tool(host->host, view(tool), execute, nullptr));
            PiHookFn hook = [](void*, PiString, PiString payload) {
                const Json in = Json::parse(std::string(payload.data, payload.size));
                return in.value("silent", false) ? PiOwnedString{nullptr, 0, nullptr}
                                                 : text(Json{{"block", true}, {"reason", "plugin says no"}}.dump());
            };
            take(host->subscribe(host->host, view(std::string("tool_call")), hook, nullptr));
            return 0;
        };
        PiPluginShutdownFn shutdown = []() { ++g_shutdowns; };
        m_libraries.provide(path, {{"pi_plugin_abi_version", reinterpret_cast<void*>(version)},
                                   {"pi_plugin_init", reinterpret_cast<void*>(init)},
                                   {"pi_plugin_shutdown", reinterpret_cast<void*>(shutdown)}});
    }

    FakeDynamicLibraries m_libraries;
    ToolRegistry m_registry;
    HookBus m_bus;
    RecordingLogger m_logger;
    std::vector<ProcessRequest> m_processRequests;
    std::function<void(const ProcessRequest&)> m_onRun;
    ScriptedProcessRunner m_runner;
    PluginHost m_host;
};

TEST_F(PluginHostTest, LoadingRegistersToolsAndHooks) {
    provideGoodPlugin("/plugins/good.so");
    EXPECT_TRUE(m_host.load({"/plugins/good.so"}).empty());
    EXPECT_EQ(m_host.loaded(), std::vector<std::string>{"/plugins/good.so"});
    EXPECT_EQ(Json::parse(g_lastReply)["ok"], true);
    const auto tool = m_registry.find("echo");
    ASSERT_NE(tool, nullptr);
    const auto result = tool->execute("c", Json{{"a", 1}}, nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<TextContent>(result->content.at(0)).text, "{\"a\":1}");
    const HookOutcome outcome = m_bus.emit("tool_call", Json{{"toolName", "bash"}});
    ASSERT_EQ(outcome.results.size(), 1U);
    EXPECT_EQ(outcome.results[0]["reason"], "plugin says no");
    EXPECT_TRUE(m_bus.emit("tool_call", Json{{"silent", true}}).results.empty());
}

TEST_F(PluginHostTest, ShutdownUnloadsInReverseAndRemovesEverything) {
    provideGoodPlugin("/plugins/good.so");
    m_host.load({"/plugins/good.so"});
    m_host.shutdown();
    m_host.shutdown();
    EXPECT_EQ(g_shutdowns, 1);
    EXPECT_EQ(m_registry.find("echo"), nullptr);
    EXPECT_FALSE(m_bus.hasHandlers("tool_call"));
    EXPECT_EQ(m_libraries.closed(), 1);
    EXPECT_TRUE(m_host.loaded().empty());
}

TEST_F(PluginHostTest, HostApiOffersContextLoggingAndExec) {
    provideGoodPlugin("/plugins/good.so");
    m_host.load({"/plugins/good.so"});
    ASSERT_NE(g_host, nullptr);
    EXPECT_EQ(g_host->abi_version, PI_PLUGIN_ABI_VERSION);
    const Json context = Json::parse(take(g_host->get_context(g_host->host)));
    EXPECT_EQ(context["cwd"], "/work");
    EXPECT_EQ(context["agentDir"], "/agent");

    const std::string message = "hello";
    g_host->log(g_host->host, PI_LOG_WARNING, view(message));
    ASSERT_EQ(m_logger.m_entries.size(), 1U);
    EXPECT_EQ(m_logger.m_entries[0].first, LogLevel::Warn);
    EXPECT_EQ(m_logger.m_entries[0].second, "[plugin good] hello");

    const std::string request = R"({"command":"git","args":["status"],"cwd":"/x","env":{"A":"1"},"stdin":"in","timeoutMs":500})";
    const Json result = Json::parse(take(g_host->exec(g_host->host, view(request), nullptr)));
    EXPECT_EQ(result["exitCode"], 3);
    EXPECT_EQ(result["output"], "ran");
    ASSERT_EQ(m_processRequests.size(), 1U);
    EXPECT_EQ(m_processRequests[0].command, "git");
    EXPECT_EQ(m_processRequests[0].args, std::vector<std::string>{"status"});
    EXPECT_EQ(*m_processRequests[0].cwd, "/x");
    EXPECT_EQ(m_processRequests[0].env.at("A"), "1");
    EXPECT_EQ(m_processRequests[0].stdinData, "in");
    EXPECT_EQ(m_processRequests[0].timeout.count(), 500);
    const std::string bad = "{}";
    EXPECT_TRUE(Json::parse(take(g_host->exec(g_host->host, view(bad), nullptr))).contains("error"));
}

TEST_F(PluginHostTest, AbortHandlesReportCancellation) {
    provideGoodPlugin("/plugins/good.so");
    m_host.load({"/plugins/good.so"});
    AbortSignal signal;
    const auto* handle = reinterpret_cast<const PiAbort*>(&signal);
    EXPECT_EQ(g_host->abort_requested(handle), 0);
    EXPECT_EQ(g_host->abort_requested(nullptr), 0);
    signal.abort();
    EXPECT_EQ(g_host->abort_requested(handle), 1);
}

TEST_F(PluginHostTest, ExecStopsWhenTheCallIsAborted) {
    provideGoodPlugin("/plugins/good.so");
    m_host.load({"/plugins/good.so"});
    AbortSignal signal;
    bool sawAbort = false;
    m_onRun = [&](const ProcessRequest& request) {
        signal.abort();
        sawAbort = request.signal->aborted();
    };
    const std::string request = R"({"command":"sleep"})";
    take(g_host->exec(g_host->host, view(request), reinterpret_cast<const PiAbort*>(&signal)));
    EXPECT_TRUE(sawAbort);
}

TEST_F(PluginHostTest, BadRegistrationsAreReportedToThePlugin) {
    provideGoodPlugin("/plugins/good.so");
    m_host.load({"/plugins/good.so"});
    const std::string bad = R"({"description":"no name"})";
    PiToolExecuteFn execute = [](void*, PiString, PiString, const PiAbort*, PiUpdateFn, void*) {
        return PiOwnedString{nullptr, 0, nullptr};
    };
    EXPECT_TRUE(Json::parse(take(g_host->register_tool(g_host->host, view(bad), execute, nullptr))).contains("error"));
    EXPECT_TRUE(Json::parse(take(g_host->subscribe(g_host->host, view(std::string()), nullptr, nullptr))).contains("error"));
}

TEST_F(PluginHostTest, InvalidHookJsonIsAnHandlerError) {
    PiPluginAbiVersionFn version = []() { return PI_PLUGIN_ABI_VERSION; };
    PiPluginInitFn init = [](const PiHostApi* host) {
        PiHookFn hook = [](void*, PiString, PiString) { return text("not json"); };
        take(host->subscribe(host->host, view(std::string("context")), hook, nullptr));
        return 0;
    };
    m_libraries.provide("/plugins/bad.so", {{"pi_plugin_abi_version", reinterpret_cast<void*>(version)},
                                            {"pi_plugin_init", reinterpret_cast<void*>(init)}});
    EXPECT_TRUE(m_host.load({"/plugins/bad.so"}).empty());
    const HookOutcome outcome = m_bus.emit("context", Json::object());
    EXPECT_EQ(outcome.errors.size(), 1U);
    EXPECT_TRUE(outcome.results.empty());
}

TEST_F(PluginHostTest, LoadFailuresAreReportedPerPlugin) {
    provideGoodPlugin("/plugins/good.so");
    PiPluginAbiVersionFn oldVersion = []() { return 99u; };
    PiPluginInitFn init = [](const PiHostApi*) { return 0; };
    m_libraries.provide("/plugins/wrong.so", {{"pi_plugin_abi_version", reinterpret_cast<void*>(oldVersion)},
                                              {"pi_plugin_init", reinterpret_cast<void*>(init)}});
    m_libraries.provide("/plugins/empty.so", {});
    PiPluginAbiVersionFn version = []() { return PI_PLUGIN_ABI_VERSION; };
    PiPluginInitFn failing = [](const PiHostApi* host) {
        const std::string tool = R"({"name":"half","description":"d"})";
        PiToolExecuteFn execute = [](void*, PiString, PiString, const PiAbort*, PiUpdateFn, void*) {
            return PiOwnedString{nullptr, 0, nullptr};
        };
        take(host->register_tool(host->host, view(tool), execute, nullptr));
        return 7;
    };
    m_libraries.provide("/plugins/failing.so", {{"pi_plugin_abi_version", reinterpret_cast<void*>(version)},
                                                {"pi_plugin_init", reinterpret_cast<void*>(failing)}});

    const auto errors = m_host.load({"/plugins/missing.so", "/plugins/wrong.so", "/plugins/empty.so",
                                     "/plugins/failing.so", "/plugins/good.so"});
    ASSERT_EQ(errors.size(), 4U);
    EXPECT_NE(errors[0].message.find("/plugins/missing.so"), std::string::npos);
    EXPECT_NE(errors[1].message.find("built for plugin ABI 99"), std::string::npos);
    EXPECT_NE(errors[2].message.find("not a pi plugin"), std::string::npos);
    EXPECT_NE(errors[3].message.find("status 7"), std::string::npos);
    EXPECT_EQ(m_registry.find("half"), nullptr);
    EXPECT_NE(m_registry.find("echo"), nullptr);
    EXPECT_EQ(m_host.loaded(), std::vector<std::string>{"/plugins/good.so"});
}

TEST_F(PluginHostTest, LoadsTheExamplePluginFromARealSharedLibrary) {
    PosixDynamicLibraries libraries;
    PluginHost host(libraries, m_registry, m_bus, m_runner, m_logger, PluginContext{"/work", "/agent"});
    const auto errors = host.load({"plugins/hello_tool/libhello_tool.so"});
    ASSERT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0].message);
    ASSERT_EQ(m_logger.m_entries.size(), 1U);
    EXPECT_EQ(m_logger.m_entries[0].second, "[plugin libhello_tool] hello plugin loaded");

    const auto tool = m_registry.find("hello");
    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->promptSnippet(), "Greet someone by name");
    std::vector<std::string> updates;
    const auto result = tool->execute("c1", Json{{"name", "Ada"}}, nullptr, [&updates](const AgentToolResult& update) {
        updates.push_back(std::get<TextContent>(update.content.at(0)).text);
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<TextContent>(result->content.at(0)).text, "Hello, Ada!");
    EXPECT_EQ(updates, std::vector<std::string>{"looking for Ada"});

    const HookOutcome blocked = m_bus.emit("tool_call", Json{{"toolName", "bash"}, {"input", Json{{"command", "rm -rf /"}}}});
    ASSERT_EQ(blocked.results.size(), 1U);
    EXPECT_EQ(blocked.results[0]["reason"], "hello plugin refuses rm -rf");
    EXPECT_TRUE(m_bus.emit("tool_call", Json{{"toolName", "bash"}, {"input", Json{{"command", "ls"}}}}).results.empty());

    host.shutdown();
    EXPECT_EQ(m_registry.find("hello"), nullptr);
}
