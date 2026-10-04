module;

#include <cstdint>
#include <cstring>

#include "pi_plugin.h"

export module pi.plugin.plugin_host;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_dynamic_libraries;
export import pi.platform.i_logger;
export import pi.platform.i_process_runner;
export import pi.plugin.i_hook_bus;
export import pi.mcp.i_mcp_server_registrar;
export import pi.plugin.i_plugin_commands;
export import pi.plugin.i_plugin_host;
export import pi.plugin.i_plugin_session_bridge;
export import pi.support.plugin_provider;
export import pi.provider.i_model_runtime;
export import pi.support.event_bus;
export import pi.support.message_codec;
export import pi.support.model_codec;
export import pi.support.plugin_tool_factory;
export import pi.support.thinking_level_resolver;
export import pi.tool.i_tool_registry;
export import pi.types.loaded_plugin;
export import pi.types.plugin_command_entry;
export import pi.types.plugin_context;

/**
 * Loads plugins through the C ABI of sdk/pi_plugin.h. Each plugin gets its own PiHostApi whose
 * `host` pointer names its LoadedPlugin, so the tools and subscriptions it registers can be
 * attributed to it and removed when it is unloaded.
 */
export class PluginHost : public IPluginHost, public IPluginCommands {
public:
    /**
     * `models` (optional) is where plugins register providers and `mcp` where they register MCP servers; `clock` stamps the
     * responses of stream-handler providers. Without them those calls fail.
     */
    PluginHost(IDynamicLibraries& libraries, IToolRegistry& tools, IHookBus& hooks, IProcessRunner& processes, ILogger& logger, PluginContext context, IModelRuntime* models = nullptr, IMcpServerRegistrar* mcp = nullptr, const IClock* clock = nullptr)
        : m_libraries(libraries),
          m_tools(tools),
          m_hooks(hooks),
          m_processes(processes),
          m_logger(logger),
          m_context(std::move(context)),
          m_models(models),
          m_mcp(mcp),
          m_clock(clock) {}

    ~PluginHost() override {
        shutdown();
    }

    std::vector<Error> load(const std::vector<std::string>& paths) override {
        std::vector<Error> errors;
        for (const std::string& path : paths) {
            if (auto loaded = loadOne(path); !loaded) {
                errors.push_back(loaded.error());
            }
        }
        return errors;
    }

    /** The session the plugins' session_call operates on; null (the initial state) answers "session not ready". */
    void setSessionBridge(IPluginSessionBridge* bridge) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_bridge = bridge;
    }

    std::vector<PluginCommandInfo> commands() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<PluginCommandInfo> out;
        for (const auto& [name, entry] : m_commands) {
            out.push_back(PluginCommandInfo{name, entry.description, entry.path});
        }
        return out;
    }

    std::optional<Result<void>> execute(const std::string& name, const std::string& args, const std::shared_ptr<AbortSignal>& abort) override {
        PluginCommandEntry entry;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_commands.find(name);
            if (found == m_commands.end()) {
                return std::nullopt;
            }
            entry = found->second;
        }
        PiOwnedString raw = entry.handler(entry.userData, PiString{args.data(), args.size()}, reinterpret_cast<const PiAbort*>(abort.get()));
        const bool empty = raw.data == nullptr || raw.size == 0;
        const std::string text = takeString(raw);
        if (empty) {
            return Result<void>{};
        }
        const Json answer = Json::parse(text, nullptr, false);
        if (answer.is_object() && answer.contains("error")) {
            return Result<void>(std::unexpected(Error{"plugin", answer["error"].is_string() ? answer["error"].get<std::string>() : "the command failed"}));
        }
        return Result<void>{};
    }

    std::vector<PluginFlag> flags() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<PluginFlag> out;
        for (const auto& [name, flag] : m_flags) {
            out.push_back(flag);
        }
        return out;
    }

    Result<void> setFlag(const std::string& name, const std::optional<std::string>& value) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_flags.find(name);
        if (found == m_flags.end()) {
            return std::unexpected(Error{"unknown_flag", "Unknown option: --" + name});
        }
        if (found->second.type == "string") {
            if (!value) {
                return std::unexpected(Error{"missing_value", "Option --" + name + " requires a value"});
            }
            m_flagValues[name] = *value;
            return {};
        }
        if (value && *value != "true" && *value != "false") {
            return std::unexpected(Error{"invalid_value", "Option --" + name + " is a switch: use true or false, or no value"});
        }
        m_flagValues[name] = !value || *value == "true";
        return {};
    }

    std::vector<std::string> loaded() const override {
        std::vector<std::string> paths;
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& plugin : m_plugins) {
            paths.push_back(plugin->path);
        }
        return paths;
    }

    void shutdown() override {
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

private:
    Result<void> loadOne(const std::string& path) {
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

    Result<void*> requiredSymbol(std::uint64_t library, const std::string& name) {
        return m_libraries.symbol(library, name);
    }

    void bindApi(LoadedPlugin& plugin) {
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
        api.register_provider = [](void* host, PiString name, PiString config) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->registerProvider(*plugin, std::string(name.data, name.size), std::string(config.data, config.size));
        };
        api.unregister_provider = [](void* host, PiString name) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->unregisterProvider(*plugin, std::string(name.data, name.size));
        };
        api.register_mcp_server = [](void* host, PiString name, PiString config) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->registerMcpServer(*plugin, std::string(name.data, name.size), std::string(config.data, config.size));
        };
        api.unregister_mcp_server = [](void* host, PiString name) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->unregisterMcpServer(*plugin, std::string(name.data, name.size));
        };
        api.register_virtual_model = [](void* host, PiString definition, PiRouteFn route, void* userData) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->registerVirtualModel(*plugin, std::string(definition.data, definition.size), route, userData);
        };
        api.unregister_virtual_model = [](void* host, PiString provider, PiString id) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->unregisterVirtualModel(*plugin, std::string(provider.data, provider.size), std::string(id.data, id.size));
        };
        api.list_models = [](void* host) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->listModels();
        };
        api.register_stream_provider = [](void* host, PiString name, PiString config, PiStreamFn stream, void* userData) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->registerStreamProvider(*plugin, std::string(name.data, name.size), std::string(config.data, config.size), stream, userData);
        };
        api.session_call = [](void* host, PiString method, PiString params, const PiAbort* abort) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->sessionCall(std::string(method.data, method.size), std::string(params.data, params.size), abort);
        };
        api.register_command = [](void* host, PiString name, PiString options, PiCommandFn handler, void* userData) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->registerCommand(*plugin, std::string(name.data, name.size), std::string(options.data, options.size), handler, userData);
        };
        api.register_flag = [](void* host, PiString name, PiString options) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->registerFlag(*plugin, std::string(name.data, name.size), std::string(options.data, options.size));
        };
        api.get_flag = [](void* host, PiString name) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->getFlag(std::string(name.data, name.size));
        };
        api.event_on = [](void* host, PiString channel, PiHookFn handler, void* userData) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->eventOn(*plugin, std::string(channel.data, channel.size), handler, userData);
        };
        api.event_emit = [](void* host, PiString channel, PiString data) {
            auto* plugin = static_cast<LoadedPlugin*>(host);
            return static_cast<PluginHost*>(plugin->owner)->eventEmit(std::string(channel.data, channel.size), std::string(data.data, data.size));
        };
        api.stream_emit = [](PiStreamSink* sink, PiString event) {
            return reinterpret_cast<PluginStreamTranslator*>(sink)->emit(std::string(event.data, event.size)) ? 0 : 1;
        };
    }

    void unload(LoadedPlugin& plugin) {
        // Streams run the plugin's code: end them before it shuts down.
        for (const auto& [name, api] : plugin.streamProviders) {
            dropStreamApi(api);
        }
        plugin.streamProviders.clear();
        if (plugin.shutdown != nullptr) {
            plugin.shutdown();
        }
        for (const std::string& name : plugin.tools) {
            m_tools.remove(name);
        }
        for (const std::uint64_t id : plugin.subscriptions) {
            m_hooks.unsubscribe(id);
        }
        if (m_models != nullptr) {
            for (const std::string& name : plugin.providers) {
                m_models->unregisterProvider(name);
            }
        }
        if (m_models != nullptr) {
            for (const auto& [provider, id] : plugin.virtualModels) {
                m_models->unregisterVirtualModel(provider, id);
            }
        }
        if (m_mcp != nullptr) {
            for (const std::string& name : plugin.mcpServers) {
                m_mcp->unregisterServer(plugin.path, name);
            }
        }
        dropCommandsAndFlags(plugin);
        for (const std::uint64_t id : plugin.eventSubscriptions) {
            m_events.off(id);
        }
        plugin.eventSubscriptions.clear();
        plugin.virtualModels.clear();
        plugin.tools.clear();
        plugin.subscriptions.clear();
        plugin.providers.clear();
        plugin.mcpServers.clear();
        m_libraries.close(plugin.library);
    }

    PiOwnedString registerTool(LoadedPlugin& plugin, const std::string& definition, PiToolExecuteFn execute, void* userData) {
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

    PiOwnedString registerProvider(LoadedPlugin& plugin, const std::string& name, const std::string& configText) {
        if (m_models == nullptr) {
            return owned(Json{{"error", "this host has no model registry"}});
        }
        const Json config = Json::parse(configText, nullptr, false);
        if (name.empty() || !config.is_object()) {
            return owned(Json{{"error", "register_provider needs a provider name and a JSON object"}});
        }
        if (auto registered = m_models->registerProvider(name, config); !registered) {
            return owned(Json{{"error", registered.error().message}});
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (std::ranges::find(plugin.providers, name) == plugin.providers.end()) {
            plugin.providers.push_back(name);
        }
        return owned(Json{{"ok", true}});
    }

    PiOwnedString unregisterProvider(LoadedPlugin& plugin, const std::string& name) {
        std::optional<std::string> api;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = std::ranges::find(plugin.providers, name);
            if (m_models == nullptr || found == plugin.providers.end()) {
                return owned(Json{{"error", "provider \"" + name + "\" was not registered by this plugin"}});
            }
            plugin.providers.erase(found);
            api = takeStreamApi(plugin, name);
        }
        m_models->unregisterProvider(name);
        if (api) {
            dropStreamApi(*api);
        }
        return owned(Json{{"ok", true}});
    }

    PiOwnedString sessionCall(const std::string& method, const std::string& paramsText, const PiAbort* abort) {
        IPluginSessionBridge* bridge = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            bridge = m_bridge;
        }
        if (bridge == nullptr) {
            return owned(Json{{"error", "session not ready"}});
        }
        Json params = Json::parse(paramsText, nullptr, false);
        if (!params.is_object()) {
            params = Json::object();
        }
        return owned(bridge->call(method, params, reinterpret_cast<const AbortSignal*>(abort)));
    }

    PiOwnedString registerCommand(LoadedPlugin& plugin, const std::string& name, const std::string& optionsText, PiCommandFn handler, void* userData) {
        const Json options = Json::parse(optionsText, nullptr, false);
        if (name.empty() || name.find_first_of(" \t\r\n/") != std::string::npos || handler == nullptr) {
            return owned(Json{{"error", "register_command needs a name without spaces or a slash and a handler"}});
        }
        PluginCommandEntry entry;
        entry.name = name;
        entry.description = options.is_object() ? options.value("description", std::string()) : std::string();
        entry.path = plugin.path;
        entry.handler = handler;
        entry.userData = userData;
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto existing = m_commands.find(name);
        if (existing != m_commands.end() && existing->second.path != plugin.path) {
            return owned(Json{{"error", "command \"" + name + "\" is already registered by another plugin"}});
        }
        m_commands[name] = std::move(entry);
        if (std::ranges::find(plugin.commands, name) == plugin.commands.end()) {
            plugin.commands.push_back(name);
        }
        return owned(Json{{"ok", true}});
    }

    PiOwnedString registerFlag(LoadedPlugin& plugin, const std::string& name, const std::string& optionsText) {
        const Json options = Json::parse(optionsText, nullptr, false);
        const std::string type = options.is_object() ? options.value("type", std::string("boolean")) : std::string("boolean");
        if (name.empty() || name.starts_with("-") || name.find_first_of(" =\t") != std::string::npos || (type != "boolean" && type != "string")) {
            return owned(Json{{"error", "register_flag needs a name without dashes, spaces or '=' and a boolean or string type"}});
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto existing = m_flags.find(name);
        if (existing != m_flags.end() && existing->second.path != plugin.path) {
            return owned(Json{{"error", "flag \"" + name + "\" is already registered by another plugin"}});
        }
        PluginFlag flag;
        flag.name = name;
        flag.description = options.is_object() ? options.value("description", std::string()) : std::string();
        flag.type = type;
        flag.defaultValue = options.is_object() && options.contains("default") ? options["default"] : Json(nullptr);
        flag.path = plugin.path;
        m_flags[name] = flag;
        if (!m_flagValues.contains(name) && !flag.defaultValue.is_null()) {
            m_flagValues[name] = flag.defaultValue;
        }
        if (std::ranges::find(plugin.flags, name) == plugin.flags.end()) {
            plugin.flags.push_back(name);
        }
        return owned(Json{{"ok", true}});
    }

    PiOwnedString getFlag(const std::string& name) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_flagValues.find(name);
        return owned(Json{{"value", found == m_flagValues.end() ? Json(nullptr) : found->second}});
    }

    PiOwnedString eventOn(LoadedPlugin& plugin, const std::string& channel, PiHookFn handler, void* userData) {
        if (channel.empty() || handler == nullptr) {
            return owned(Json{{"error", "event_on needs a channel and a handler"}});
        }
        const std::uint64_t id = m_events.on(channel, [this, handler, userData](const std::string& name, const Json& data) {
            const std::string text = data.dump(-1, ' ', false, Json::error_handler_t::replace);
            PiOwnedString raw = handler(userData, PiString{name.data(), name.size()}, PiString{text.data(), text.size()});
            takeString(raw);
        });
        const std::lock_guard<std::mutex> lock(m_mutex);
        plugin.eventSubscriptions.push_back(id);
        return owned(Json{{"ok", true}});
    }

    PiOwnedString eventEmit(const std::string& channel, const std::string& dataText) {
        const Json data = Json::parse(dataText, nullptr, false);
        m_events.emit(channel, data.is_discarded() ? Json(nullptr) : data);
        return owned(Json{{"ok", true}});
    }

    /** Forgets the commands and flags a plugin registered; m_mutex is not held. */
    void dropCommandsAndFlags(LoadedPlugin& plugin) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const std::string& name : plugin.commands) {
            const auto found = m_commands.find(name);
            if (found != m_commands.end() && found->second.path == plugin.path) {
                m_commands.erase(found);
            }
        }
        for (const std::string& name : plugin.flags) {
            const auto found = m_flags.find(name);
            if (found != m_flags.end() && found->second.path == plugin.path) {
                m_flags.erase(found);
            }
        }
        plugin.commands.clear();
        plugin.flags.clear();
    }

    PiOwnedString registerStreamProvider(LoadedPlugin& plugin, const std::string& name, const std::string& configText, PiStreamFn stream, void* userData) {
        if (m_models == nullptr || m_clock == nullptr) {
            return owned(Json{{"error", "this host has no model registry"}});
        }
        const Json config = Json::parse(configText, nullptr, false);
        if (name.empty() || stream == nullptr || !config.is_object() || !config.contains("api") || !config["api"].is_string() || config["api"].get<std::string>().empty()) {
            return owned(Json{{"error", "register_stream_provider needs a provider name, a stream function and a JSON object naming its \"api\""}});
        }
        const std::string api = config["api"].get<std::string>();
        // A plugin registering the same provider again replaces its earlier stream function.
        std::optional<std::string> previous;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            previous = takeStreamApi(plugin, name);
        }
        if (previous) {
            dropStreamApi(*previous);
        }
        auto provider = std::make_shared<PluginProvider>(api, stream, userData, *m_clock);
        if (auto added = m_models->registerApi(provider); !added) {
            return owned(Json{{"error", added.error().message}});
        }
        if (auto registered = m_models->registerProvider(name, config); !registered) {
            m_models->unregisterApi(api);
            return owned(Json{{"error", registered.error().message}});
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_streamProviders[api] = provider;
        plugin.streamProviders.emplace_back(name, api);
        if (std::ranges::find(plugin.providers, name) == plugin.providers.end()) {
            plugin.providers.push_back(name);
        }
        return owned(Json{{"ok", true}});
    }

    /** Forgets that `plugin` provides `name` with a stream function and returns its API; the caller holds m_mutex. */
    std::optional<std::string> takeStreamApi(LoadedPlugin& plugin, const std::string& name) {
        const auto found = std::ranges::find_if(plugin.streamProviders, [&name](const std::pair<std::string, std::string>& entry) { return entry.first == name; });
        if (found == plugin.streamProviders.end()) {
            return std::nullopt;
        }
        std::string api = found->second;
        plugin.streamProviders.erase(found);
        return api;
    }

    /** Withdraws the API of a stream provider and ends its running streams (waits for them: call without locks held). */
    void dropStreamApi(const std::string& api) {
        std::shared_ptr<PluginProvider> provider;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_streamProviders.find(api);
            if (found == m_streamProviders.end()) {
                return;
            }
            provider = std::move(found->second);
            m_streamProviders.erase(found);
        }
        if (m_models != nullptr) {
            m_models->unregisterApi(api);
        }
        provider->shutdown();
    }

    PiOwnedString registerMcpServer(LoadedPlugin& plugin, const std::string& name, const std::string& configText) {
        if (m_mcp == nullptr) {
            return owned(Json{{"error", "this host does not connect MCP servers"}});
        }
        const Json config = Json::parse(configText, nullptr, false);
        if (name.empty() || !config.is_object()) {
            return owned(Json{{"error", "register_mcp_server needs a server name and a JSON object"}});
        }
        if (auto registered = m_mcp->registerServer(plugin.path, name, config); !registered) {
            return owned(Json{{"error", registered.error().message}});
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (std::ranges::find(plugin.mcpServers, name) == plugin.mcpServers.end()) {
            plugin.mcpServers.push_back(name);
        }
        return owned(Json{{"ok", true}});
    }

    PiOwnedString unregisterMcpServer(LoadedPlugin& plugin, const std::string& name) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = std::ranges::find(plugin.mcpServers, name);
            if (m_mcp == nullptr || found == plugin.mcpServers.end()) {
                return owned(Json{{"error", "MCP server \"" + name + "\" was not registered by this plugin"}});
            }
            plugin.mcpServers.erase(found);
        }
        m_mcp->unregisterServer(plugin.path, name);
        return owned(Json{{"ok", true}});
    }

    PiOwnedString registerVirtualModel(LoadedPlugin& plugin, const std::string& definitionText, PiRouteFn route, void* userData) {
        if (m_models == nullptr) {
            return owned(Json{{"error", "this host has no model registry"}});
        }
        const Json json = Json::parse(definitionText, nullptr, false);
        if (route == nullptr || !json.is_object() || !json.contains("provider") || !json["provider"].is_string() || !json.contains("id") || !json["id"].is_string()) {
            return owned(Json{{"error", "register_virtual_model needs a definition with provider and id and a route function"}});
        }
        VirtualModelDefinition definition;
        definition.provider = json["provider"].get<std::string>();
        definition.id = json["id"].get<std::string>();
        definition.name = json.contains("name") && json["name"].is_string() ? json["name"].get<std::string>() : definition.id;
        if (json.contains("thinkingLevels") && json["thinkingLevels"].is_array()) {
            for (const Json& name : json["thinkingLevels"]) {
                const auto level = name.is_string() ? m_levels.parseLevel(name.get<std::string>()) : std::nullopt;
                if (!level) {
                    return owned(Json{{"error", "unknown thinking level in thinkingLevels"}});
                }
                definition.thinkingLevels.push_back(*level);
            }
        }
        if (json.contains("contextWindow") && json["contextWindow"].is_number_integer()) {
            definition.contextWindow = json["contextWindow"].get<std::int64_t>();
        }
        if (json.contains("maxTokens") && json["maxTokens"].is_number_integer()) {
            definition.maxTokens = json["maxTokens"].get<std::int64_t>();
        }
        if (json.contains("input") && json["input"].is_array()) {
            for (const Json& kind : json["input"]) {
                if (kind.is_string()) {
                    definition.input.push_back(kind.get<std::string>());
                }
            }
        }
        const std::string provider = definition.provider;
        const std::string id = definition.id;
        definition.route = [this, route, userData](const VirtualRouteRequest& request) { return callRoute(route, userData, request); };
        if (auto registered = m_models->registerVirtualModel(std::move(definition)); !registered) {
            return owned(Json{{"error", registered.error().message}});
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        const std::pair<std::string, std::string> key{provider, id};
        if (std::ranges::find(plugin.virtualModels, key) == plugin.virtualModels.end()) {
            plugin.virtualModels.push_back(key);
        }
        return owned(Json{{"ok", true}});
    }

    PiOwnedString unregisterVirtualModel(LoadedPlugin& plugin, const std::string& provider, const std::string& id) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = std::ranges::find(plugin.virtualModels, std::pair<std::string, std::string>{provider, id});
        if (m_models == nullptr || found == plugin.virtualModels.end()) {
            return owned(Json{{"error", "virtual model \"" + provider + "/" + id + "\" was not registered by this plugin"}});
        }
        plugin.virtualModels.erase(found);
        m_models->unregisterVirtualModel(provider, id);
        return owned(Json{{"ok", true}});
    }

    PiOwnedString listModels() {
        Json models = Json::array();
        if (m_models != nullptr) {
            for (const Model& model : m_models->availableModels()) {
                if (model.api == "pi-virtual") {
                    continue;
                }
                models.push_back(Json{{"provider", model.provider}, {"id", model.id}, {"name", model.name}, {"reasoning", model.reasoning}, {"input", model.input}, {"contextWindow", model.contextWindow}, {"maxTokens", model.maxTokens}});
            }
        }
        return owned(models);
    }

    /** Asks the plugin's router; the request and the answer travel as JSON (see PiRouteFn). */
    Result<VirtualRoute> callRoute(PiRouteFn route, void* userData, const VirtualRouteRequest& request) const {
        Json json = Json::object({{"model", m_modelCodec.toJson(request.model)},
                                  {"thinkingLevel", m_levels.levelName(request.thinkingLevel)},
                                  {"reason", request.reason},
                                  {"messages", m_messageCodec.toJson(request.messages)}});
        if (request.previous) {
            json["previous"] = selectionJson(request.previous->model, request.previous->thinkingLevel);
        }
        if (request.failed) {
            Json failed = selectionJson(request.failed->model, request.failed->thinkingLevel);
            failed["message"] = m_messageCodec.toJson(request.failed->message);
            json["failed"] = std::move(failed);
        }
        if (!request.state.is_null()) {
            json["state"] = request.state;
        }
        const std::string text = json.dump(-1, ' ', false, Json::error_handler_t::replace);
        PiOwnedString raw = route(userData, PiString{text.data(), text.size()}, reinterpret_cast<const PiAbort*>(request.signal.get()));
        const bool empty = raw.data == nullptr || raw.size == 0;
        const std::string answerText = takeString(raw);
        if (empty) {
            return std::unexpected(Error{"plugin", "the router returned nothing"});
        }
        const Json answer = Json::parse(answerText, nullptr, false);
        if (!answer.is_object()) {
            return std::unexpected(Error{"plugin", "the router returned invalid JSON"});
        }
        if (answer.contains("error")) {
            return std::unexpected(Error{"plugin", answer["error"].is_string() ? answer["error"].get<std::string>() : "the router failed"});
        }
        if (!answer.contains("model") || !answer["model"].is_object() || !answer["model"].contains("provider") || !answer["model"]["provider"].is_string() || !answer["model"].contains("id") || !answer["model"]["id"].is_string()) {
            return std::unexpected(Error{"plugin", "the router must answer {model: {provider, id}, thinkingLevel}"});
        }
        VirtualRoute out;
        out.model.provider = answer["model"]["provider"].get<std::string>();
        out.model.id = answer["model"]["id"].get<std::string>();
        const auto level = answer.contains("thinkingLevel") && answer["thinkingLevel"].is_string() ? m_levels.parseLevel(answer["thinkingLevel"].get<std::string>()) : std::optional<ThinkingLevel>(ThinkingLevel::Off);
        if (!level) {
            return std::unexpected(Error{"plugin", "the router answered an unknown thinking level"});
        }
        out.thinkingLevel = *level;
        if (answer.contains("state") && answer["state"] != request.state) {
            out.state = answer["state"];
        }
        return out;
    }

    Json selectionJson(const Model& model, const std::optional<ThinkingLevel>& level) const {
        Json out = Json::object({{"model", m_modelCodec.toJson(model)}});
        if (level) {
            out["thinkingLevel"] = m_levels.levelName(*level);
        }
        return out;
    }

    PiOwnedString subscribe(LoadedPlugin& plugin, const std::string& event, PiHookFn handler, void* userData) {
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

    PiOwnedString execute(const std::string& requestText, const PiAbort* abort) {
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

    PiOwnedString context() const {
        return owned(Json{{"cwd", m_context.cwd}, {"agentDir", m_context.agentDir}});
    }

    void log(const LoadedPlugin& plugin, int level, const std::string& message) {
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

    Result<Json> callHook(PiHookFn handler, void* userData, const std::string& event, const Json& payload) const {
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

    PiOwnedString owned(const Json& json) const {
        const std::string text = json.dump(-1, ' ', false, Json::error_handler_t::replace);
        char* copy = new char[text.size()];
        std::memcpy(copy, text.data(), text.size());
        return PiOwnedString{copy, text.size(), [](char* data, std::size_t) { delete[] data; }};
    }

    std::string takeString(PiOwnedString& value) const {
        std::string text = value.data != nullptr ? std::string(value.data, value.size) : std::string();
        if (value.release != nullptr) {
            value.release(value.data, value.size);
        }
        value.data = nullptr;
        return text;
    }

    std::string fileStem(const std::string& path) const {
        const std::size_t slash = path.find_last_of('/');
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        const std::size_t dot = name.find('.');
        return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    }

    IDynamicLibraries& m_libraries;
    IToolRegistry& m_tools;
    IHookBus& m_hooks;
    IProcessRunner& m_processes;
    ILogger& m_logger;
    PluginContext m_context;
    PluginToolFactory m_factory;
    ModelCodec m_modelCodec;
    MessageCodec m_messageCodec;
    ThinkingLevelResolver m_levels;
    IModelRuntime* m_models;
    IMcpServerRegistrar* m_mcp;
    const IClock* m_clock;
    mutable std::mutex m_mutex;
    /** Stream-handler providers by API name. */
    std::map<std::string, std::shared_ptr<PluginProvider>> m_streamProviders;
    IPluginSessionBridge* m_bridge = nullptr;
    std::map<std::string, PluginCommandEntry> m_commands;
    std::map<std::string, PluginFlag> m_flags;
    std::map<std::string, Json> m_flagValues;
    EventBus m_events;
    std::vector<std::unique_ptr<LoadedPlugin>> m_plugins;
};
