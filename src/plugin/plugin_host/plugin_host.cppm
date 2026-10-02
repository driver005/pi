module;

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>

#include "pi_plugin.h"

export module pi.plugin.plugin_host;

import std;
export import pi.platform.i_dynamic_libraries;
export import pi.platform.i_logger;
export import pi.platform.i_process_runner;
export import pi.plugin.i_hook_bus;
export import pi.plugin.i_plugin_host;
export import pi.support.plugin_tool_factory;
export import pi.tool.i_tool_registry;
export import pi.types.loaded_plugin;
export import pi.types.plugin_context;

/**
 * Loads plugins through the C ABI of sdk/pi_plugin.h. Each plugin gets its own PiHostApi whose
 * `host` pointer names its LoadedPlugin, so the tools and subscriptions it registers can be
 * attributed to it and removed when it is unloaded.
 */
export class PluginHost : public IPluginHost {
public:
    PluginHost(IDynamicLibraries& libraries, IToolRegistry& tools, IHookBus& hooks, IProcessRunner& processes,
               ILogger& logger, PluginContext context);
    ~PluginHost() override;

    std::vector<Error> load(const std::vector<std::string>& paths) override;
    std::vector<std::string> loaded() const override;
    void shutdown() override;

private:
    Result<void> loadOne(const std::string& path);
    Result<void*> requiredSymbol(std::uint64_t library, const std::string& name);
    void bindApi(LoadedPlugin& plugin);
    void unload(LoadedPlugin& plugin);
    PiOwnedString registerTool(LoadedPlugin& plugin, const std::string& definition, PiToolExecuteFn execute,
                               void* userData);
    PiOwnedString subscribe(LoadedPlugin& plugin, const std::string& event, PiHookFn handler, void* userData);
    PiOwnedString execute(const std::string& request, const PiAbort* abort);
    PiOwnedString context() const;
    void log(const LoadedPlugin& plugin, int level, const std::string& message);
    Result<Json> callHook(PiHookFn handler, void* userData, const std::string& event, const Json& payload) const;
    PiOwnedString owned(const Json& json) const;
    std::string takeString(PiOwnedString& value) const;
    std::string fileStem(const std::string& path) const;

    IDynamicLibraries& m_libraries;
    IToolRegistry& m_tools;
    IHookBus& m_hooks;
    IProcessRunner& m_processes;
    ILogger& m_logger;
    PluginContext m_context;
    PluginToolFactory m_factory;
    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<LoadedPlugin>> m_plugins;
};

PluginHost::PluginHost(IDynamicLibraries& libraries, IToolRegistry& tools, IHookBus& hooks,
                       IProcessRunner& processes, ILogger& logger, PluginContext context)
    : m_libraries(libraries),
      m_tools(tools),
      m_hooks(hooks),
      m_processes(processes),
      m_logger(logger),
      m_context(std::move(context)) {}

PluginHost::~PluginHost() {
    shutdown();
}

std::string PluginHost::fileStem(const std::string& path) const {
    const std::size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

PiOwnedString PluginHost::owned(const Json& json) const {
    const std::string text = json.dump(-1, ' ', false, Json::error_handler_t::replace);
    char* copy = new char[text.size()];
    std::memcpy(copy, text.data(), text.size());
    return PiOwnedString{copy, text.size(), [](char* data, std::size_t) { delete[] data; }};
}

std::string PluginHost::takeString(PiOwnedString& value) const {
    std::string text = value.data != nullptr ? std::string(value.data, value.size) : std::string();
    if (value.release != nullptr) {
        value.release(value.data, value.size);
    }
    value.data = nullptr;
    return text;
}

void PluginHost::log(const LoadedPlugin& plugin, int level, const std::string& message) {
    LogLevel mapped = LogLevel::Info;
    if (level <= PI_LOG_DEBUG) {
        mapped = LogLevel::Debug;
    } else if (level == PI_LOG_WARNING) {
        mapped = LogLevel::Warn;
    } else if (level >= PI_LOG_ERROR) {
        mapped = LogLevel::Error;
    }
    m_logger.log(mapped, "[plugin " + plugin.name + "] " + message);
}

PiOwnedString PluginHost::context() const {
    return owned(Json{{"cwd", m_context.cwd}, {"agentDir", m_context.agentDir}});
}

PiOwnedString PluginHost::registerTool(LoadedPlugin& plugin, const std::string& definition,
                                       PiToolExecuteFn execute, void* userData) {
    auto tool = m_factory.create(definition, execute, userData);
    if (!tool) {
        return owned(Json{{"error", tool.error().message}});
    }
    const std::string name = (*tool)->definition().name;
    m_tools.add(*tool);
    const std::lock_guard<std::mutex> lock(m_mutex);
    plugin.tools.push_back(name);
    return owned(Json{{"ok", true}});
}

Result<Json> PluginHost::callHook(PiHookFn handler, void* userData, const std::string& event,
                                  const Json& payload) const {
    const std::string payloadText = payload.dump(-1, ' ', false, Json::error_handler_t::replace);
    PiOwnedString raw = handler(userData, PiString{event.data(), event.size()},
                                PiString{payloadText.data(), payloadText.size()});
    const bool empty = raw.data == nullptr || raw.size == 0;
    const std::string text = takeString(raw);
    if (empty) {
        return Json();
    }
    Json result = Json::parse(text, nullptr, false);
    if (result.is_discarded()) {
        return std::unexpected(Error{"plugin", "hook \"" + event + "\" returned invalid JSON"});
    }
    return result;
}

PiOwnedString PluginHost::subscribe(LoadedPlugin& plugin, const std::string& event, PiHookFn handler,
                                    void* userData) {
    if (event.empty() || handler == nullptr) {
        return owned(Json{{"error", "subscribe needs an event name and a handler"}});
    }
    const std::uint64_t id = m_hooks.subscribe(
        event, [this, handler, userData](const std::string& name, const Json& payload) {
            return callHook(handler, userData, name, payload);
        });
    const std::lock_guard<std::mutex> lock(m_mutex);
    plugin.subscriptions.push_back(id);
    return owned(Json{{"ok", true}});
}

PiOwnedString PluginHost::execute(const std::string& requestText, const PiAbort* abort) {
    const Json request = Json::parse(requestText, nullptr, false);
    if (!request.is_object() || !request.contains("command") || !request["command"].is_string()) {
        return owned(Json{{"error", "exec needs a command"}});
    }
    ProcessRequest process;
    process.command = request["command"].get<std::string>();
    if (request.contains("args") && request["args"].is_array()) {
        for (const Json& arg : request["args"]) {
            if (arg.is_string()) {
                process.args.push_back(arg.get<std::string>());
            }
        }
    }
    if (request.contains("cwd") && request["cwd"].is_string()) {
        process.cwd = request["cwd"].get<std::string>();
    }
    if (request.contains("env") && request["env"].is_object()) {
        for (const auto& entry : request["env"].items()) {
            if (entry.value().is_string()) {
                process.env[entry.key()] = entry.value().get<std::string>();
            }
        }
    }
    process.stdinData = request.value("stdin", "");
    if (request.contains("timeoutMs") && request["timeoutMs"].is_number_integer()) {
        process.timeout = std::chrono::milliseconds(request["timeoutMs"].get<std::int64_t>());
    }
    // The plugin's abort handle is the AbortSignal of the tool call (see PluginTool).
    auto* callSignal = const_cast<AbortSignal*>(reinterpret_cast<const AbortSignal*>(abort));
    process.signal = std::make_shared<AbortSignal>();
    const std::uint64_t link =
        callSignal != nullptr ? callSignal->onAbort([signal = process.signal]() { signal->abort(); }) : 0;
    const auto result = m_processes.run(process);
    if (callSignal != nullptr) {
        callSignal->removeListener(link);
    }
    if (!result) {
        return owned(Json{{"error", result.error().message}});
    }
    return owned(Json{{"exitCode", result->exitCode},
                      {"output", result->output},
                      {"timedOut", result->timedOut},
                      {"aborted", result->aborted}});
}

void PluginHost::bindApi(LoadedPlugin& plugin) {
    plugin.owner = this;
    PiHostApi& api = plugin.api;
    api.abi_version = PI_PLUGIN_ABI_VERSION;
    api.struct_size = sizeof(PiHostApi);
    api.host = &plugin;
    api.log = [](void* host, int level, PiString message) {
        auto* plugin = static_cast<LoadedPlugin*>(host);
        static_cast<PluginHost*>(plugin->owner)->log(*plugin, level, std::string(message.data, message.size));
    };
    api.register_tool = [](void* host, PiString definition, PiToolExecuteFn execute, void* userData) {
        auto* plugin = static_cast<LoadedPlugin*>(host);
        return static_cast<PluginHost*>(plugin->owner)
            ->registerTool(*plugin, std::string(definition.data, definition.size), execute, userData);
    };
    api.subscribe = [](void* host, PiString event, PiHookFn handler, void* userData) {
        auto* plugin = static_cast<LoadedPlugin*>(host);
        return static_cast<PluginHost*>(plugin->owner)
            ->subscribe(*plugin, std::string(event.data, event.size), handler, userData);
    };
    api.abort_requested = [](const PiAbort* abort) {
        return abort != nullptr && reinterpret_cast<const AbortSignal*>(abort)->aborted() ? 1 : 0;
    };
    api.exec = [](void* host, PiString request, const PiAbort* abort) {
        auto* plugin = static_cast<LoadedPlugin*>(host);
        return static_cast<PluginHost*>(plugin->owner)->execute(std::string(request.data, request.size), abort);
    };
    api.get_context = [](void* host) {
        auto* plugin = static_cast<LoadedPlugin*>(host);
        return static_cast<PluginHost*>(plugin->owner)->context();
    };
}

Result<void*> PluginHost::requiredSymbol(std::uint64_t library, const std::string& name) {
    return m_libraries.symbol(library, name);
}

void PluginHost::unload(LoadedPlugin& plugin) {
    if (plugin.shutdown != nullptr) {
        plugin.shutdown();
    }
    for (const std::string& name : plugin.tools) {
        m_tools.remove(name);
    }
    for (const std::uint64_t id : plugin.subscriptions) {
        m_hooks.unsubscribe(id);
    }
    plugin.tools.clear();
    plugin.subscriptions.clear();
    m_libraries.close(plugin.library);
}

Result<void> PluginHost::loadOne(const std::string& path) {
    auto library = m_libraries.open(path);
    if (!library) {
        return std::unexpected(Error{"plugin", path + ": " + library.error().message});
    }
    auto versionSymbol = requiredSymbol(*library, "pi_plugin_abi_version");
    auto initSymbol = requiredSymbol(*library, "pi_plugin_init");
    if (!versionSymbol || !initSymbol) {
        m_libraries.close(*library);
        return std::unexpected(Error{"plugin", path + ": not a pi plugin (needs pi_plugin_abi_version and pi_plugin_init)"});
    }
    const std::uint32_t version = reinterpret_cast<PiPluginAbiVersionFn>(*versionSymbol)();
    if (version != PI_PLUGIN_ABI_VERSION) {
        m_libraries.close(*library);
        return std::unexpected(Error{"plugin", path + ": built for plugin ABI " + std::to_string(version) +
                                                   ", this host supports " + std::to_string(PI_PLUGIN_ABI_VERSION)});
    }
    auto plugin = std::make_unique<LoadedPlugin>();
    plugin->path = path;
    plugin->name = fileStem(path);
    plugin->library = *library;
    if (auto shutdownSymbol = requiredSymbol(*library, "pi_plugin_shutdown")) {
        plugin->shutdown = reinterpret_cast<PiPluginShutdownFn>(*shutdownSymbol);
    }
    bindApi(*plugin);
    LoadedPlugin* raw = plugin.get();
    const int status = reinterpret_cast<PiPluginInitFn>(*initSymbol)(&raw->api);
    if (status != 0) {
        unload(*raw);
        return std::unexpected(Error{"plugin", path + ": pi_plugin_init failed with status " + std::to_string(status)});
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_plugins.push_back(std::move(plugin));
    return {};
}

std::vector<Error> PluginHost::load(const std::vector<std::string>& paths) {
    std::vector<Error> errors;
    for (const std::string& path : paths) {
        if (auto loaded = loadOne(path); !loaded) {
            errors.push_back(loaded.error());
        }
    }
    return errors;
}

std::vector<std::string> PluginHost::loaded() const {
    std::vector<std::string> paths;
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& plugin : m_plugins) {
        paths.push_back(plugin->path);
    }
    return paths;
}

void PluginHost::shutdown() {
    std::vector<std::unique_ptr<LoadedPlugin>> plugins;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        plugins = std::move(m_plugins);
        m_plugins.clear();
    }
    for (auto it = plugins.rbegin(); it != plugins.rend(); ++it) {
        unload(**it);
    }
}
