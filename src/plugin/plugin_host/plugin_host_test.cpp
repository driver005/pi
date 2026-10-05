#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "pi_plugin.h"

import std;
import pi.base.posix_dynamic_libraries;
import pi.plugin.plugin_host;
import pi.support.hook_bus;
import pi.testing.fake_dynamic_libraries;
import pi.testing.fake_model_runtime;
import pi.testing.fixed_clock;
import pi.testing.scripted_process_runner;
import pi.tools.tool_registry;

// The fake plugins below are plain C functions, so what they observe lives in globals.
const PiHostApi* g_host = nullptr;
std::string g_lastLog;
std::string g_lastReply;
int g_shutdowns = 0;
std::string g_providerConfig;
std::string g_serverConfig;

class RecordingRegistrar : public IMcpServerRegistrar {
public:
    Result<void> registerServer(const std::string& owner, const std::string& name, const Json& config) override {
        if (m_refuse) {
            return std::unexpected(Error{"mcp_server_conflict", "already registered"});
        }
        m_servers[name] = {owner, config};
        return {};
    }
    void unregisterServer(const std::string& owner, const std::string& name) override {
        const auto found = m_servers.find(name);
        if (found != m_servers.end() && found->second.first == owner) {
            m_servers.erase(found);
        }
    }

    std::map<std::string, std::pair<std::string, Json>> m_servers;
    bool m_refuse = false;
};

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
          m_host(m_libraries, m_registry, m_bus, m_runner, m_logger, PluginContext{"/work", "/agent"}, &m_models, &m_servers) {
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
    FakeModelRuntime m_models;
    RecordingRegistrar m_servers;
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

class PluginHostProviderTest : public PluginHostTest {
protected:
    /** A plugin that registers the provider "proxy" with the config in `g_providerConfig` and records the replies. */
    void provideProviderPlugin(const std::string& path) {
        PiPluginAbiVersionFn version = []() { return PI_PLUGIN_ABI_VERSION; };
        PiPluginInitFn init = [](const PiHostApi* host) {
            g_host = host;
            const std::string name = "proxy";
            g_lastReply = take(host->register_provider(host->host, view(name), view(g_providerConfig)));
            return 0;
        };
        PiPluginShutdownFn shutdown = []() { ++g_shutdowns; };
        m_libraries.provide(path, {{"pi_plugin_abi_version", reinterpret_cast<void*>(version)},
                                   {"pi_plugin_init", reinterpret_cast<void*>(init)},
                                   {"pi_plugin_shutdown", reinterpret_cast<void*>(shutdown)}});
    }
};

TEST_F(PluginHostProviderTest, APluginRegistersAProviderThatEndsWithThePlugin) {
    g_providerConfig = R"({"baseUrl":"https://proxy.example.com","api":"openai-completions","apiKey":"k","models":[{"id":"m1"}]})";
    provideProviderPlugin("/plugins/provider.so");
    EXPECT_TRUE(m_host.load({"/plugins/provider.so"}).empty());
    EXPECT_EQ(Json::parse(g_lastReply)["ok"], true);
    ASSERT_EQ(m_models.registeredProviders().size(), 1u);
    EXPECT_EQ(m_models.registeredProviders().at("proxy")["baseUrl"], "https://proxy.example.com");
    m_host.shutdown();
    EXPECT_TRUE(m_models.registeredProviders().empty());
}

TEST_F(PluginHostProviderTest, TheRegistrysRejectionIsReportedToThePlugin) {
    m_models.rejectProviders();
    g_providerConfig = R"({"baseUrl":"x"})";
    provideProviderPlugin("/plugins/provider.so");
    EXPECT_TRUE(m_host.load({"/plugins/provider.so"}).empty());
    EXPECT_EQ(Json::parse(g_lastReply)["error"], "rejected by the test");
    EXPECT_TRUE(m_models.registeredProviders().empty());
}

TEST_F(PluginHostProviderTest, ConfigsThatAreNotObjectsAreRefused) {
    g_providerConfig = "[1]";
    provideProviderPlugin("/plugins/provider.so");
    EXPECT_TRUE(m_host.load({"/plugins/provider.so"}).empty());
    EXPECT_TRUE(Json::parse(g_lastReply).contains("error"));
    EXPECT_TRUE(m_models.registeredProviders().empty());
}

TEST_F(PluginHostProviderTest, APluginCanUnregisterItsOwnProviderOnly) {
    g_providerConfig = R"({"baseUrl":"https://proxy.example.com"})";
    provideProviderPlugin("/plugins/provider.so");
    ASSERT_TRUE(m_host.load({"/plugins/provider.so"}).empty());
    const std::string other = "someone-else";
    EXPECT_TRUE(Json::parse(take(g_host->unregister_provider(g_host->host, view(other)))).contains("error"));
    const std::string mine = "proxy";
    EXPECT_EQ(Json::parse(take(g_host->unregister_provider(g_host->host, view(mine))))["ok"], true);
    EXPECT_TRUE(m_models.registeredProviders().empty());
}

std::string g_virtualDefinition;
std::string g_routeRequest;
std::string g_routeAnswer;
bool g_routeEmpty = false;

class PluginHostVirtualModelTest : public PluginHostTest {
protected:
    /** A plugin that registers the virtual model in `g_virtualDefinition`; its router records the request and answers `g_routeAnswer`. */
    void provideRouterPlugin(const std::string& path) {
        PiPluginAbiVersionFn version = []() { return PI_PLUGIN_ABI_VERSION; };
        PiPluginInitFn init = [](const PiHostApi* host) {
            g_host = host;
            PiRouteFn route = [](void*, PiString request, const PiAbort*) {
                g_routeRequest.assign(request.data, request.size);
                return g_routeEmpty ? PiOwnedString{nullptr, 0, nullptr} : text(g_routeAnswer);
            };
            g_lastReply = take(host->register_virtual_model(host->host, view(g_virtualDefinition), route, nullptr));
            return 0;
        };
        PiPluginShutdownFn shutdown = []() { ++g_shutdowns; };
        m_libraries.provide(path, {{"pi_plugin_abi_version", reinterpret_cast<void*>(version)},
                                   {"pi_plugin_init", reinterpret_cast<void*>(init)},
                                   {"pi_plugin_shutdown", reinterpret_cast<void*>(shutdown)}});
    }

    void SetUp() override {
        g_virtualDefinition = R"({"provider":"router","id":"auto","name":"Auto","thinkingLevels":["low","high"],"contextWindow":1000})";
        g_routeAnswer = R"({"model":{"provider":"openai","id":"gpt-a"},"thinkingLevel":"high","state":{"phase":2}})";
        g_routeRequest.clear();
        g_routeEmpty = false;
        Model physical;
        physical.provider = "openai";
        physical.id = "gpt-a";
        physical.api = "faux";
        physical.name = "GPT A";
        m_models.addModel(physical);
        m_models.setAuthenticated("openai", true);
    }

    VirtualResolveRequest request() {
        VirtualResolveRequest out;
        out.model = *m_models.find("router", "auto");
        out.thinkingLevel = ThinkingLevel::High;
        out.reason = "continuation";
        out.state = Json{{"phase", 1}};
        UserMessage user;
        user.content = std::string("hi");
        out.messages = {Message(user)};
        return out;
    }
};

TEST_F(PluginHostVirtualModelTest, APluginRegistersAVirtualModelWhoseRouterReceivesTheRequest) {
    provideRouterPlugin("/plugins/router.so");
    ASSERT_TRUE(m_host.load({"/plugins/router.so"}).empty());
    EXPECT_EQ(Json::parse(g_lastReply)["ok"], true);
    const auto model = m_models.find("router", "auto");
    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(model->contextWindow, 1000);
    EXPECT_TRUE(model->reasoning);

    const auto route = m_models.resolveVirtual(request());
    ASSERT_TRUE(route.has_value()) << route.error().message;
    EXPECT_EQ(route->model.id, "gpt-a");
    EXPECT_EQ(route->thinkingLevel, ThinkingLevel::Off);
    ASSERT_TRUE(route->state.has_value());
    EXPECT_EQ((*route->state)["phase"], 2);
    const Json seen = Json::parse(g_routeRequest);
    EXPECT_EQ(seen["model"]["id"], "auto");
    EXPECT_EQ(seen["thinkingLevel"], "high");
    EXPECT_EQ(seen["reason"], "continuation");
    EXPECT_EQ(seen["state"]["phase"], 1);
    EXPECT_EQ(seen["messages"][0]["role"], "user");
    EXPECT_FALSE(seen.contains("previous"));
    EXPECT_FALSE(seen.contains("failed"));

    m_host.shutdown();
    EXPECT_FALSE(m_models.find("router", "auto").has_value());
}

TEST_F(PluginHostVirtualModelTest, AnUnchangedStateIsNotStoredAgain) {
    g_routeAnswer = R"({"model":{"provider":"openai","id":"gpt-a"},"state":{"phase":1}})";
    provideRouterPlugin("/plugins/router.so");
    ASSERT_TRUE(m_host.load({"/plugins/router.so"}).empty());
    const auto route = m_models.resolveVirtual(request());
    ASSERT_TRUE(route.has_value());
    EXPECT_FALSE(route->state.has_value());
}

TEST_F(PluginHostVirtualModelTest, RoutersThatFailFailTheRequest) {
    provideRouterPlugin("/plugins/router.so");
    ASSERT_TRUE(m_host.load({"/plugins/router.so"}).empty());
    g_routeAnswer = R"({"error":"classifier down"})";
    auto route = m_models.resolveVirtual(request());
    ASSERT_FALSE(route.has_value());
    EXPECT_NE(route.error().message.find("classifier down"), std::string::npos);
    g_routeAnswer = R"({"model":{"provider":"openai"}})";
    route = m_models.resolveVirtual(request());
    ASSERT_FALSE(route.has_value());
    EXPECT_NE(route.error().message.find("must answer"), std::string::npos);
    g_routeAnswer = R"({"model":{"provider":"openai","id":"gpt-a"},"thinkingLevel":"sideways"})";
    route = m_models.resolveVirtual(request());
    ASSERT_FALSE(route.has_value());
    EXPECT_NE(route.error().message.find("unknown thinking level"), std::string::npos);
    g_routeEmpty = true;
    route = m_models.resolveVirtual(request());
    ASSERT_FALSE(route.has_value());
    EXPECT_NE(route.error().message.find("returned nothing"), std::string::npos);
}

TEST_F(PluginHostVirtualModelTest, BadDefinitionsAndConflictsAreReportedToThePlugin) {
    g_virtualDefinition = R"({"provider":"router"})";
    provideRouterPlugin("/plugins/router.so");
    ASSERT_TRUE(m_host.load({"/plugins/router.so"}).empty());
    EXPECT_TRUE(Json::parse(g_lastReply).contains("error"));
    m_host.shutdown();
    g_virtualDefinition = R"({"provider":"openai","id":"gpt-a"})";
    provideRouterPlugin("/plugins/router2.so");
    ASSERT_TRUE(m_host.load({"/plugins/router2.so"}).empty());
    EXPECT_NE(Json::parse(g_lastReply)["error"].get<std::string>().find("conflicts with a physical model"), std::string::npos);
    m_host.shutdown();
    g_virtualDefinition = R"({"provider":"router","id":"auto","thinkingLevels":["bogus"]})";
    provideRouterPlugin("/plugins/router3.so");
    ASSERT_TRUE(m_host.load({"/plugins/router3.so"}).empty());
    EXPECT_NE(Json::parse(g_lastReply)["error"].get<std::string>().find("thinking level"), std::string::npos);
}

TEST_F(PluginHostVirtualModelTest, APluginListsThePhysicalModelsWithCredentialsAndUnregistersItsOwn) {
    provideRouterPlugin("/plugins/router.so");
    ASSERT_TRUE(m_host.load({"/plugins/router.so"}).empty());
    Model other;
    other.provider = "groq";
    other.id = "llama";
    other.api = "faux";
    m_models.addModel(other);
    const Json models = Json::parse(take(g_host->list_models(g_host->host)));
    ASSERT_EQ(models.size(), 1U);
    EXPECT_EQ(models[0]["provider"], "openai");
    EXPECT_EQ(models[0]["id"], "gpt-a");
    const std::string provider = "router";
    const std::string foreign = "other";
    const std::string id = "auto";
    EXPECT_TRUE(Json::parse(take(g_host->unregister_virtual_model(g_host->host, view(provider), view(foreign)))).contains("error"));
    EXPECT_EQ(Json::parse(take(g_host->unregister_virtual_model(g_host->host, view(provider), view(id))))["ok"], true);
    EXPECT_FALSE(m_models.find("router", "auto").has_value());
}

TEST_F(PluginHostTest, WithoutAModelRegistryProvidersCannotBeRegistered) {
    FakeDynamicLibraries libraries;
    ToolRegistry registry;
    HookBus bus;
    RecordingLogger logger;
    ScriptedProcessRunner runner([](const ProcessRequest&) -> Result<ProcessResult> { return ProcessResult{}; });
    PluginHost host(libraries, registry, bus, runner, logger, PluginContext{"/work", "/agent"});
    PiPluginAbiVersionFn version = []() { return PI_PLUGIN_ABI_VERSION; };
    PiPluginInitFn init = [](const PiHostApi* api) {
        const std::string name = "p";
        const std::string config = "{}";
        g_lastReply = take(api->register_provider(api->host, view(name), view(config)));
        return 0;
    };
    libraries.provide("/plugins/p.so", {{"pi_plugin_abi_version", reinterpret_cast<void*>(version)}, {"pi_plugin_init", reinterpret_cast<void*>(init)}});
    EXPECT_TRUE(host.load({"/plugins/p.so"}).empty());
    EXPECT_EQ(Json::parse(g_lastReply)["error"], "this host has no model registry");
}

class PluginHostMcpTest : public PluginHostTest {
protected:
    void provideMcpPlugin(const std::string& path) {
        PiPluginAbiVersionFn version = []() { return PI_PLUGIN_ABI_VERSION; };
        PiPluginInitFn init = [](const PiHostApi* host) {
            g_host = host;
            const std::string name = "jira";
            g_lastReply = take(host->register_mcp_server(host->host, view(name), view(g_serverConfig)));
            return 0;
        };
        PiPluginShutdownFn shutdown = []() { ++g_shutdowns; };
        m_libraries.provide(path, {{"pi_plugin_abi_version", reinterpret_cast<void*>(version)},
                                   {"pi_plugin_init", reinterpret_cast<void*>(init)},
                                   {"pi_plugin_shutdown", reinterpret_cast<void*>(shutdown)}});
    }
};

TEST_F(PluginHostMcpTest, APluginRegistersAnMcpServerThatEndsWithThePlugin) {
    g_serverConfig = R"({"url":"https://mcp.example.com/jira"})";
    provideMcpPlugin("/plugins/mcp.so");
    EXPECT_TRUE(m_host.load({"/plugins/mcp.so"}).empty());
    EXPECT_EQ(Json::parse(g_lastReply)["ok"], true);
    ASSERT_EQ(m_servers.m_servers.size(), 1u);
    EXPECT_EQ(m_servers.m_servers.at("jira").first, "/plugins/mcp.so");
    EXPECT_EQ(m_servers.m_servers.at("jira").second["url"], "https://mcp.example.com/jira");
    m_host.shutdown();
    EXPECT_TRUE(m_servers.m_servers.empty());
}

TEST_F(PluginHostMcpTest, TheRegistrarsRefusalIsReportedAndNothingIsTracked) {
    m_servers.m_refuse = true;
    g_serverConfig = R"({"url":"https://mcp.example.com/jira"})";
    provideMcpPlugin("/plugins/mcp.so");
    EXPECT_TRUE(m_host.load({"/plugins/mcp.so"}).empty());
    EXPECT_EQ(Json::parse(g_lastReply)["error"], "already registered");
}

TEST_F(PluginHostMcpTest, ConfigsThatAreNotObjectsAreRefused) {
    g_serverConfig = "7";
    provideMcpPlugin("/plugins/mcp.so");
    EXPECT_TRUE(m_host.load({"/plugins/mcp.so"}).empty());
    EXPECT_TRUE(Json::parse(g_lastReply).contains("error"));
    EXPECT_TRUE(m_servers.m_servers.empty());
}

TEST_F(PluginHostMcpTest, APluginUnregistersItsOwnServerOnly) {
    g_serverConfig = R"({"url":"https://mcp.example.com/jira"})";
    provideMcpPlugin("/plugins/mcp.so");
    ASSERT_TRUE(m_host.load({"/plugins/mcp.so"}).empty());
    const std::string other = "other";
    EXPECT_TRUE(Json::parse(take(g_host->unregister_mcp_server(g_host->host, view(other)))).contains("error"));
    const std::string mine = "jira";
    EXPECT_EQ(Json::parse(take(g_host->unregister_mcp_server(g_host->host, view(mine))))["ok"], true);
    EXPECT_TRUE(m_servers.m_servers.empty());
}

class PluginHostStreamTest : public PluginHostTest {
protected:
    PluginHostStreamTest()
        : m_streamHost(m_realLibraries, m_registry, m_bus, m_runner, m_logger, PluginContext{"/work", "/agent"}, &m_models, nullptr, &m_clock) {}

    ~PluginHostStreamTest() override {
        m_streamHost.shutdown();
    }

    /** The first user message and the model the provider is asked to stream. */
    std::shared_ptr<AssistantMessageStream> ask(const std::string& text, std::shared_ptr<AbortSignal> signal = nullptr) {
        const auto provider = m_models.registeredApis().at("hello-stream-api");
        Model model;
        model.id = "echo";
        model.provider = "hello-stream";
        model.api = "hello-stream-api";
        TranscriptContext context;
        UserMessage user;
        TextContent block;
        block.text = text;
        user.content = std::vector<UserContentBlock>{block};
        context.messages.push_back(user);
        StreamOptions options;
        options.signal = std::move(signal);
        return provider->stream(model, context, options);
    }

    /** A plugin that registers a stream provider with the config in `g_providerConfig`. */
    void provideStreamPlugin(const std::string& path) {
        PiPluginAbiVersionFn version = []() { return PI_PLUGIN_ABI_VERSION; };
        PiPluginInitFn init = [](const PiHostApi* host) {
            g_host = host;
            const std::string name = "stream-proxy";
            PiStreamFn stream = [](void*, PiString, const PiAbort*, PiStreamSink* sink) {
                const std::string done = R"({"type":"done"})";
                g_host->stream_emit(sink, view(done));
            };
            g_lastReply = take(host->register_stream_provider(host->host, view(name), view(g_providerConfig), stream, nullptr));
            return 0;
        };
        m_libraries.provide(path, {{"pi_plugin_abi_version", reinterpret_cast<void*>(version)}, {"pi_plugin_init", reinterpret_cast<void*>(init)}});
    }

    FixedClock m_clock;
    PosixDynamicLibraries m_realLibraries;
    PluginHost m_streamHost;
};

TEST_F(PluginHostStreamTest, TheExampleStreamProviderAnswersRequestsThroughTheSink) {
    ASSERT_TRUE(m_streamHost.load({"plugins/hello_stream/libhello_stream.so"}).empty());
    EXPECT_EQ(m_models.registeredProviders().at("hello-stream")["api"], "hello-stream-api");
    EXPECT_EQ(m_models.registeredApis().count("hello-stream-api"), 1U);

    const auto reply = ask("hello world")->result();
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->stopReason, StopReason::Stop);
    ASSERT_EQ(reply->content.size(), 2U);
    EXPECT_EQ(std::get<ThinkingContent>(reply->content[0]).thinking, "echoing");
    EXPECT_EQ(std::get<TextContent>(reply->content[1]).text, "You said: hello world ");
    EXPECT_EQ(reply->usage.input, 10);
    EXPECT_EQ(reply->responseId, "echo-1");

    const auto call = ask("call read notes.txt")->result();
    ASSERT_TRUE(call.has_value());
    EXPECT_EQ(call->stopReason, StopReason::ToolUse);
    const ToolCall& tool = std::get<ToolCall>(call->content[0]);
    EXPECT_EQ(tool.name, "read");
    EXPECT_EQ(tool.arguments["input"], "notes.txt");

    const auto failed = ask("fail")->result();
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(failed->stopReason, StopReason::Error);
    EXPECT_EQ(failed->errorMessage, "the echo model failed");
}

TEST_F(PluginHostStreamTest, UnloadingCancelsRunningStreamsAndWithdrawsTheProvider) {
    ASSERT_TRUE(m_streamHost.load({"plugins/hello_stream/libhello_stream.so"}).empty());
    const auto slow = ask("slow");
    const auto first = slow->next();
    ASSERT_TRUE(first.has_value());
    m_streamHost.shutdown();
    EXPECT_EQ(slow->result()->stopReason, StopReason::Aborted);
    EXPECT_TRUE(m_models.registeredApis().empty());
    EXPECT_TRUE(m_models.registeredProviders().empty());
}

TEST_F(PluginHostStreamTest, TheRequestsSignalCancelsAStream) {
    ASSERT_TRUE(m_streamHost.load({"plugins/hello_stream/libhello_stream.so"}).empty());
    const auto signal = std::make_shared<AbortSignal>();
    const auto slow = ask("slow", signal);
    ASSERT_TRUE(slow->next().has_value());
    signal->abort();
    EXPECT_EQ(slow->result()->stopReason, StopReason::Aborted);
}

TEST_F(PluginHostStreamTest, BadRegistrationsAreReportedToThePlugin) {
    PluginHost host(m_libraries, m_registry, m_bus, m_runner, m_logger, PluginContext{"/work", "/agent"}, &m_models, nullptr, &m_clock);
    provideStreamPlugin("/plugins/s.so");
    g_providerConfig = R"({"apiKey":"none"})";
    EXPECT_TRUE(host.load({"/plugins/s.so"}).empty());
    EXPECT_TRUE(Json::parse(g_lastReply).contains("error")) << "no api named";
    host.shutdown();
    g_providerConfig = R"({"api":"stream-api","apiKey":"none","models":[{"id":"m"}]})";
    EXPECT_TRUE(host.load({"/plugins/s.so"}).empty());
    EXPECT_EQ(Json::parse(g_lastReply)["ok"], true);
    EXPECT_EQ(m_models.registeredApis().count("stream-api"), 1U);
    EXPECT_EQ(m_models.registeredProviders().count("stream-proxy"), 1U);

    PluginHost other(m_libraries, m_registry, m_bus, m_runner, m_logger, PluginContext{"/work", "/agent"}, &m_models, nullptr, &m_clock);
    EXPECT_TRUE(other.load({"/plugins/s.so"}).empty());
    EXPECT_NE(Json::parse(g_lastReply)["error"].get<std::string>().find("already implemented"), std::string::npos);

    g_providerConfig = R"({"api":"stream-api","apiKey":"none"})";
    host.shutdown();
    EXPECT_TRUE(m_models.registeredApis().empty());
}

TEST_F(PluginHostStreamTest, RegisteringAgainReplacesAndUnregisteringWithdrawsTheApi) {
    PluginHost host(m_libraries, m_registry, m_bus, m_runner, m_logger, PluginContext{"/work", "/agent"}, &m_models, nullptr, &m_clock);
    provideStreamPlugin("/plugins/s.so");
    g_providerConfig = R"({"api":"stream-api","apiKey":"none","models":[{"id":"m"}]})";
    ASSERT_TRUE(host.load({"/plugins/s.so"}).empty());
    const std::string name = "stream-proxy";
    const std::string api = "stream-api";
    const std::string config = g_providerConfig;
    PiStreamFn stream = [](void*, PiString, const PiAbort*, PiStreamSink*) {};
    EXPECT_EQ(Json::parse(take(g_host->register_stream_provider(g_host->host, view(name), view(config), stream, nullptr)))["ok"], true);
    EXPECT_EQ(m_models.registeredApis().count("stream-api"), 1U);
    EXPECT_EQ(Json::parse(take(g_host->unregister_provider(g_host->host, view(name))))["ok"], true);
    EXPECT_TRUE(m_models.registeredApis().empty());
    EXPECT_TRUE(m_models.registeredProviders().empty());
}

TEST_F(PluginHostStreamTest, WithoutAClockStreamProvidersCannotBeRegistered) {
    provideStreamPlugin("/plugins/s.so");
    g_providerConfig = R"({"api":"stream-api","apiKey":"none"})";
    EXPECT_TRUE(m_host.load({"/plugins/s.so"}).empty());
    EXPECT_EQ(Json::parse(g_lastReply)["error"], "this host has no model registry");
}

std::string g_oauthProvider;
std::vector<int> g_uiResults;
std::string g_prompted;
std::string g_refreshInput;
std::string g_commandArgs;
std::string g_commandSession;
std::string g_eventData;
std::string g_eventChannel;

class RecordingBridge : public IPluginSessionBridge {
public:
    Json call(const std::string& method, const Json& params, const AbortSignal*) override {
        m_calls.emplace_back(method, params);
        return Json{{"echo", method}};
    }

    std::vector<std::pair<std::string, Json>> m_calls;
};

class PluginHostCommandsTest : public PluginHostTest {
protected:
    PluginHostCommandsTest() {
        g_commandArgs.clear();
        g_commandSession.clear();
        g_eventData.clear();
        g_eventChannel.clear();
    }

    void provideCommandsPlugin(const std::string& path, bool second = false) {
        PiPluginAbiVersionFn version = []() { return PI_PLUGIN_ABI_VERSION; };
        PiPluginInitFn init = [](const PiHostApi* host) {
            g_host = host;
            PiCommandFn handler = [](void*, PiString args, const PiAbort*) {
                g_commandArgs.assign(args.data, args.size);
                if (g_commandArgs == "session") {
                    const std::string method = "isIdle";
                    const std::string params = "{}";
                    g_commandSession = take(g_host->session_call(g_host->host, view(method), view(params), nullptr));
                }
                return g_commandArgs == "boom" ? text(R"({"error":"it broke"})") : PiOwnedString{nullptr, 0, nullptr};
            };
            const std::string name = "greet";
            const std::string options = R"({"description":"Greets"})";
            g_lastReply = take(host->register_command(host->host, view(name), view(options), handler, nullptr));
            const std::string loud = "loud";
            const std::string loudOptions = R"({"type":"boolean","description":"Shout"})";
            take(host->register_flag(host->host, view(loud), view(loudOptions)));
            const std::string who = "who";
            const std::string whoOptions = R"({"type":"string","default":"world"})";
            take(host->register_flag(host->host, view(who), view(whoOptions)));
            PiHookFn onEvent = [](void*, PiString channel, PiString data) {
                g_eventChannel.assign(channel.data, channel.size);
                g_eventData.assign(data.data, data.size);
                return PiOwnedString{nullptr, 0, nullptr};
            };
            const std::string chan = "chan";
            take(host->event_on(host->host, view(chan), onEvent, nullptr));
            return 0;
        };
        PiPluginInitFn conflicting = [](const PiHostApi* host) {
            g_host = host;
            PiCommandFn handler = [](void*, PiString, const PiAbort*) { return PiOwnedString{nullptr, 0, nullptr}; };
            const std::string name = "greet";
            g_lastReply = take(host->register_command(host->host, view(name), view(std::string("{}")), handler, nullptr));
            return 0;
        };
        PiPluginShutdownFn shutdown = []() { ++g_shutdowns; };
        m_libraries.provide(path, {{"pi_plugin_abi_version", reinterpret_cast<void*>(version)},
                                   {"pi_plugin_init", reinterpret_cast<void*>(second ? conflicting : init)},
                                   {"pi_plugin_shutdown", reinterpret_cast<void*>(shutdown)}});
    }

    std::shared_ptr<AbortSignal> m_signal = std::make_shared<AbortSignal>();
};

TEST_F(PluginHostCommandsTest, ACommandRunsWithItsArgumentsAndAnErrorIsReturned) {
    provideCommandsPlugin("/plugins/c.so");
    ASSERT_TRUE(m_host.load({"/plugins/c.so"}).empty());
    EXPECT_EQ(Json::parse(g_lastReply)["ok"], true);
    ASSERT_EQ(m_host.commands().size(), 1U);
    EXPECT_EQ(m_host.commands()[0].name, "greet");
    EXPECT_EQ(m_host.commands()[0].description, "Greets");
    EXPECT_EQ(m_host.commands()[0].path, "/plugins/c.so");

    const auto ran = m_host.execute("greet", "to you", m_signal, nullptr);
    ASSERT_TRUE(ran.has_value());
    EXPECT_TRUE(ran->has_value());
    EXPECT_EQ(g_commandArgs, "to you");

    const auto broke = m_host.execute("greet", "boom", m_signal, nullptr);
    ASSERT_TRUE(broke.has_value());
    ASSERT_FALSE(broke->has_value());
    EXPECT_EQ(broke->error().message, "it broke");

    EXPECT_FALSE(m_host.execute("unknown", "", m_signal, nullptr).has_value());
}

TEST_F(PluginHostCommandsTest, CommandNamesAreUniqueAndEndWithThePlugin) {
    provideCommandsPlugin("/plugins/c.so");
    provideCommandsPlugin("/plugins/other.so", true);
    ASSERT_TRUE(m_host.load({"/plugins/c.so", "/plugins/other.so"}).empty());
    EXPECT_NE(Json::parse(g_lastReply)["error"].get<std::string>().find("already registered"), std::string::npos);
    EXPECT_EQ(m_host.commands().size(), 1U);
    m_host.shutdown();
    EXPECT_TRUE(m_host.commands().empty());
    EXPECT_TRUE(m_host.flags().empty());
}

TEST_F(PluginHostCommandsTest, FlagsTakeValuesFromTheCommandLine) {
    provideCommandsPlugin("/plugins/c.so");
    ASSERT_TRUE(m_host.load({"/plugins/c.so"}).empty());
    EXPECT_EQ(m_host.flags().size(), 2U);
    const auto read = [](const std::string& name) { return Json::parse(take(g_host->get_flag(g_host->host, view(name))))["value"]; };
    EXPECT_TRUE(read("loud").is_null());
    EXPECT_EQ(read("who"), "world");

    EXPECT_TRUE(m_host.setFlag("loud", std::nullopt).has_value());
    EXPECT_EQ(read("loud"), true);
    EXPECT_TRUE(m_host.setFlag("loud", "false").has_value());
    EXPECT_EQ(read("loud"), false);
    EXPECT_FALSE(m_host.setFlag("loud", "maybe").has_value());
    EXPECT_TRUE(m_host.setFlag("who", "you").has_value());
    EXPECT_EQ(read("who"), "you");
    EXPECT_FALSE(m_host.setFlag("who", std::nullopt).has_value());
    const auto unknown = m_host.setFlag("nope", std::nullopt);
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().message, "Unknown option: --nope");
}

TEST_F(PluginHostCommandsTest, SessionCallsAnswerNotReadyUntilTheBridgeIsSet) {
    provideCommandsPlugin("/plugins/c.so");
    ASSERT_TRUE(m_host.load({"/plugins/c.so"}).empty());
    const std::string method = "isIdle";
    const std::string params = "{}";
    EXPECT_EQ(Json::parse(take(g_host->session_call(g_host->host, view(method), view(params), nullptr)))["error"], "session not ready");

    RecordingBridge bridge;
    m_host.setSessionBridge(&bridge);
    EXPECT_EQ(Json::parse(take(g_host->session_call(g_host->host, view(method), view(params), nullptr)))["echo"], "isIdle");
    const std::string bad = "[1]";
    take(g_host->session_call(g_host->host, view(method), view(bad), nullptr));
    ASSERT_EQ(bridge.m_calls.size(), 2U);
    EXPECT_TRUE(bridge.m_calls[1].second.is_object()) << "non-object params become {}";
    m_host.setSessionBridge(nullptr);
    EXPECT_EQ(Json::parse(take(g_host->session_call(g_host->host, view(method), view(params), nullptr)))["error"], "session not ready");
}

TEST_F(PluginHostCommandsTest, ACommandOperatesOnTheBridgeItWasRunWith) {
    provideCommandsPlugin("/plugins/c.so");
    ASSERT_TRUE(m_host.load({"/plugins/c.so"}).empty());
    RecordingBridge own;
    RecordingBridge shared;
    m_host.setSessionBridge(&shared);

    ASSERT_TRUE(m_host.execute("greet", "session", m_signal, &own).has_value());
    EXPECT_EQ(Json::parse(g_commandSession)["echo"], "isIdle");
    EXPECT_EQ(own.m_calls.size(), 1U);
    EXPECT_TRUE(shared.m_calls.empty());

    ASSERT_TRUE(m_host.execute("greet", "session", m_signal, nullptr).has_value());
    EXPECT_EQ(own.m_calls.size(), 1U) << "the per-command bridge is gone after the command";
    EXPECT_EQ(shared.m_calls.size(), 1U);
}

TEST_F(PluginHostCommandsTest, TheEventBusReachesSubscribers) {
    provideCommandsPlugin("/plugins/c.so");
    ASSERT_TRUE(m_host.load({"/plugins/c.so"}).empty());
    const std::string channel = "chan";
    const std::string data = R"({"n":1})";
    EXPECT_EQ(Json::parse(take(g_host->event_emit(g_host->host, view(channel), view(data))))["ok"], true);
    EXPECT_EQ(g_eventChannel, "chan");
    EXPECT_EQ(g_eventData, data);
}

class PluginHostOauthTest : public PluginHostTest {
protected:
    void provideOauthPlugin(const std::string& path, const std::string& provider) {
        g_oauthProvider = provider;
        PiPluginAbiVersionFn version = []() { return PI_PLUGIN_ABI_VERSION; };
        PiPluginInitFn init = [](const PiHostApi* host) {
            g_host = host;
            PiOauthLoginFn login = [](void*, PiOauthUi* ui, const PiAbort*) {
                const std::string url = "https://acme.test/login";
                const std::string instructions = "go there";
                g_uiResults.push_back(g_host->oauth_auth(g_host->host, ui, view(url), view(instructions)));
                const std::string message = "waiting";
                g_uiResults.push_back(g_host->oauth_progress(g_host->host, ui, view(message)));
                const std::string code = "Code:";
                g_uiResults.push_back(g_host->oauth_device_code(g_host->host, ui, view(code), view(url)));
                const std::string asked = "Paste the code";
                g_prompted = take(g_host->oauth_prompt(g_host->host, ui, view(asked)));
                return text(Json{{"access", "a"}, {"refresh", "r"}, {"expires", 123}, {"extra", "kept"}}.dump());
            };
            PiOauthRefreshFn refresh = [](void*, PiString credential, const PiAbort*) {
                g_refreshInput.assign(credential.data, credential.size);
                return text(Json{{"access", "a2"}, {"refresh", "r"}, {"expires", 456}}.dump());
            };
            const std::string options = R"({"name":"Acme","subscription":true})";
            g_lastReply = take(host->register_oauth(host->host, view(g_oauthProvider), view(options), login, refresh, nullptr));
            return 0;
        };
        PiPluginShutdownFn shutdown = []() { ++g_shutdowns; };
        m_libraries.provide(path, {{"pi_plugin_abi_version", reinterpret_cast<void*>(version)},
                                   {"pi_plugin_init", reinterpret_cast<void*>(init)},
                                   {"pi_plugin_shutdown", reinterpret_cast<void*>(shutdown)}});
    }
};

TEST_F(PluginHostOauthTest, ASignInIsRegisteredWithTheRuntimeAndRunsThroughThePlugin) {
    g_uiResults.clear();
    g_prompted.clear();
    provideOauthPlugin("/plugins/o.so", "acme");
    ASSERT_TRUE(m_host.load({"/plugins/o.so"}).empty());
    EXPECT_EQ(Json::parse(g_lastReply)["ok"], true);
    ASSERT_EQ(m_models.registeredOauthFlows().count("acme"), 1U);
    EXPECT_EQ(m_host.oauthProviders(), (std::vector<std::pair<std::string, std::string>>{{"acme", "Acme"}}));

    LoginInteraction interaction;
    std::vector<std::string> shown;
    interaction.authUrl = [&shown](const std::string& url, const std::string&) { shown.push_back(url); };
    interaction.progress = [&shown](const std::string& message) { shown.push_back(message); };
    interaction.deviceCode = [&shown](const std::string& code, const std::string&, std::optional<std::int64_t>, std::optional<std::int64_t>) { shown.push_back(code); };
    interaction.manualCode = [](const std::string&) -> std::optional<std::string> { return "pasted"; };
    const auto credential = m_host.oauthLogin("acme", interaction);
    ASSERT_TRUE(credential.has_value());
    EXPECT_EQ(shown, (std::vector<std::string>{"https://acme.test/login", "waiting", "Code:"}));
    EXPECT_EQ(g_uiResults, (std::vector<int>{0, 0, 0}));
    EXPECT_EQ(Json::parse(g_prompted)["value"], "pasted");
    EXPECT_EQ(credential->access, "a");
    EXPECT_EQ(credential->extra["extra"], "kept");

    IOauthFlow* flow = m_models.registeredOauthFlows().at("acme");
    EXPECT_TRUE(flow->isSubscription());
    EXPECT_EQ(flow->name(), "Acme");
    const auto refreshed = flow->refresh(*credential, nullptr);
    ASSERT_TRUE(refreshed.has_value());
    EXPECT_EQ(refreshed->access, "a2");
    EXPECT_EQ(Json::parse(g_refreshInput)["extra"], "kept");
    EXPECT_EQ(flow->toAuth(*refreshed).apiKey, "a2");

    EXPECT_EQ(m_host.oauthLogin("other", interaction).error().code, "unknown_provider");
}

TEST_F(PluginHostOauthTest, ASignInWithoutAPromptHandlerAnswersNullAndCancelledSignInsReportIt) {
    g_uiResults.clear();
    g_prompted.clear();
    provideOauthPlugin("/plugins/o.so", "acme");
    ASSERT_TRUE(m_host.load({"/plugins/o.so"}).empty());
    LoginInteraction interaction;
    interaction.signal = std::make_shared<AbortSignal>();
    interaction.signal->abort();
    interaction.authUrl = [](const std::string&, const std::string&) {};
    ASSERT_TRUE(m_host.oauthLogin("acme", interaction).has_value());
    EXPECT_EQ(g_uiResults.front(), 1) << "an aborted sign-in tells the plugin";
    EXPECT_TRUE(Json::parse(g_prompted)["value"].is_null());
}

TEST_F(PluginHostOauthTest, OnlyOnePluginSignsInToAProviderAndTheFlowEndsWithThePlugin) {
    provideOauthPlugin("/plugins/o.so", "acme");
    provideOauthPlugin("/plugins/p.so", "acme");
    ASSERT_TRUE(m_host.load({"/plugins/o.so", "/plugins/p.so"}).empty());
    EXPECT_NE(Json::parse(g_lastReply)["error"].get<std::string>().find("already has an OAuth sign-in"), std::string::npos);
    EXPECT_EQ(m_models.registeredOauthFlows().size(), 1U);
    m_host.shutdown();
    EXPECT_TRUE(m_models.registeredOauthFlows().empty());
    EXPECT_TRUE(m_host.oauthProviders().empty());
}

TEST_F(PluginHostOauthTest, RegisteringNeedsAModelRegistryAndValidArguments) {
    PluginHost bare(m_libraries, m_registry, m_bus, m_runner, m_logger, PluginContext{"/work", "/agent"}, nullptr, nullptr);
    provideOauthPlugin("/plugins/o.so", "acme");
    ASSERT_TRUE(bare.load({"/plugins/o.so"}).empty());
    EXPECT_EQ(Json::parse(g_lastReply)["error"], "this host has no model registry");
    bare.shutdown();

    provideOauthPlugin("/plugins/q.so", "");
    ASSERT_TRUE(m_host.load({"/plugins/q.so"}).empty());
    EXPECT_TRUE(Json::parse(g_lastReply).contains("error"));
    EXPECT_TRUE(m_models.registeredOauthFlows().empty());
}
