module;

#include <cstdint>

export module pi.ai.model_runtime;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_file_system;
export import pi.provider.i_credential_store;
export import pi.provider.i_model_runtime;
export import pi.provider.i_models_store;
export import pi.provider.i_oauth_flow;
export import pi.provider.i_provider_registry;
export import pi.support.builtin_provider_table;
export import pi.support.config_value_resolver;
export import pi.support.env_key_table;
export import pi.support.error_stream_factory;
export import pi.support.header_merger;
export import pi.support.model_catalog_loader;
export import pi.support.model_composer;
export import pi.support.models_config_loader;
export import pi.support.provider_auth_resolver;
export import pi.support.virtual_model_registry;
export import pi.types.model_runtime_config;
export import pi.types.prepared_request;
export import pi.types.provider_state;

/**
 * IModelRuntime over the built-in provider table, the generated catalog (plus remote overlays in
 * the models store), the user's models.json and plugin-registered providers. Port of the parts of
 * packages/coding-agent/src/core/model-runtime.ts that a headless backbone needs. Virtual models registered by plugins are listed
 * with the physical models of their provider (hiding a physical model of the same id) and route through VirtualModelRegistry.
 */
export class ModelRuntime : public IModelRuntime {
public:
    ModelRuntime(ModelRuntimeConfig config, ICredentialStore& credentials, IModelsStore& store, IFileSystem& files, IProviderRegistry& providers, const EnvKeyTable& envKeys, ConfigValueResolver& configValues, const IClock& clock, std::map<std::string, IOauthFlow*> oauthFlows)
        : m_config(std::move(config)),
          m_credentials(credentials),
          m_store(store),
          m_providers(providers),
          m_envKeys(envKeys),
          m_configValues(configValues),
          m_clock(clock),
          m_auth(credentials, envKeys, configValues, clock, std::move(oauthFlows)),
          m_catalogLoader(files),
          m_modelsConfigLoader(files) {}

    Result<void> reload() override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_error.reset();
        ModelMap base = loadBaseModels();
        const std::int64_t generatedAt =
            m_config.catalogDir.empty() ? 0 : m_catalogLoader.generatedAtMs(m_config.catalogDir);
        overlayRemote(base, generatedAt);
        m_baseModels = base;
        m_modelsJson = Json::object();
        if (!m_config.modelsJsonPath.empty()) {
            auto loaded = m_modelsConfigLoader.load(m_config.modelsJsonPath);
            if (loaded) {
                m_modelsJson = std::move(*loaded);
            } else {
                setError(loaded.error().message);
            }
        }
        buildProviders(m_baseModels, m_modelsJson);
        return {};
    }

    std::optional<std::string> error() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_error;
    }

    std::vector<Model> models() const override {
        const std::vector<Model> virtualModels = m_virtual.models();
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Model> all;
        for (const auto& [id, state] : m_states) {
            for (const Model& model : state.models) {
                if (!hiddenByVirtual(virtualModels, model)) {
                    all.push_back(model);
                }
            }
        }
        all.insert(all.end(), virtualModels.begin(), virtualModels.end());
        return all;
    }

    std::vector<Model> availableModels() override {
        std::vector<Model> available;
        for (const auto& model : models()) {
            if (hasConfiguredAuth(model.provider)) {
                available.push_back(model);
            }
        }
        return available;
    }

    std::optional<Model> find(const std::string& provider, const std::string& id) const override {
        if (auto virtualModel = m_virtual.find(provider, id)) {
            return virtualModel;
        }
        return physicalModel(provider, id);
    }

    std::optional<Model> physicalModel(const std::string& provider, const std::string& id) const override {
        const auto found = state(provider);
        if (!found) {
            return std::nullopt;
        }
        for (const auto& model : found->models) {
            if (model.id == id) {
                return model;
            }
        }
        return std::nullopt;
    }

    std::string providerName(const std::string& provider) const override {
        const auto found = state(provider);
        return found ? found->definition.name : provider;
    }

    std::vector<std::string> providerIds() const override {
        const std::set<std::string> virtualProviders = m_virtual.providers();
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> ids;
        for (const auto& [id, state] : m_states) {
            ids.push_back(id);
        }
        for (const std::string& id : virtualProviders) {
            if (!m_states.contains(id)) {
                ids.push_back(id);
            }
        }
        return ids;
    }

    AuthStatus authStatus(const std::string& provider) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_runtimeKeys.contains(provider)) {
                return AuthStatus{true, "runtime", ""};
            }
        }
        if (auto infos = m_credentials.list()) {
            for (const auto& info : *infos) {
                if (info.providerId == provider) {
                    return AuthStatus{true, "stored", ""};
                }
            }
        }
        const auto found = state(provider);
        if (!found) {
            return m_virtual.listsProvider(provider) ? AuthStatus{true, "virtual", ""} : AuthStatus{};
        }
        const std::string rawKey = found->config.is_object() && found->config.contains("apiKey") &&
                                           found->config["apiKey"].is_string()
                                       ? found->config["apiKey"].get<std::string>()
                                       : "";
        if (!rawKey.empty()) {
            if (m_configValues.isCommand(rawKey)) {
                return AuthStatus{true, "models_json_command", ""};
            }
            const auto names = m_configValues.envVarNames(rawKey);
            if (!names.empty()) {
                if (!m_configValues.isConfigured(rawKey, {})) {
                    return AuthStatus{};
                }
                std::string label;
                for (std::size_t i = 0; i < names.size(); ++i) {
                    label += (i > 0 ? ", " : "") + names[i];
                }
                return AuthStatus{true, "environment", label};
            }
            return AuthStatus{true, "models_json_key", ""};
        }
        for (const auto& name : found->definition.envVars) {
            if (m_envKeys.value(name, {})) {
                return AuthStatus{true, "environment", name};
            }
        }
        if (const auto ambient = m_envKeys.apiKey(provider, {})) {
            return AuthStatus{true, "ambient", ""};
        }
        return AuthStatus{};
    }

    bool hasConfiguredAuth(const std::string& provider) override {
        return authStatus(provider).configured;
    }

    void setRuntimeApiKey(const std::string& provider, const std::string& apiKey) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_runtimeKeys[provider] = apiKey;
    }

    void removeRuntimeApiKey(const std::string& provider) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_runtimeKeys.erase(provider);
    }

    Result<std::optional<AuthResult>> getAuth(const std::string& provider, const std::optional<std::string>& apiKeyOverride, const std::map<std::string, std::string>& env) override {
        return resolveAuth(provider, apiKeyOverride, env);
    }

    std::shared_ptr<AssistantMessageStream> stream(const Model& model, const TranscriptContext& context, const StreamOptions& options) override {
        if (m_virtual.isVirtual(model)) {
            return m_errors.failed(model, "Virtual model " + model.provider + "/" + model.id + " must be routed before streaming", m_clock.nowMs());
        }
        auto prepared = prepare(model, options);
        if (!prepared) {
            return m_errors.failed(model, prepared.error().message, m_clock.nowMs());
        }
        auto provider = m_providers.find(model.api);
        if (!provider) {
            return m_errors.failed(model, "No API provider registered for api: " + model.api, m_clock.nowMs());
        }
        return provider->stream(prepared->model, context, prepared->options);
    }

    std::shared_ptr<AssistantMessageStream> fetchDeferred(const Model& model, const DeferredHandle& handle, const StreamOptions& options) override {
        auto prepared = prepare(model, options);
        if (!prepared) {
            return m_errors.failed(model, prepared.error().message, m_clock.nowMs());
        }
        auto provider = m_providers.find(model.api);
        if (!provider || !provider->supportsDeferred()) {
            return m_errors.failed(model, "Provider " + model.provider + " does not support deferred responses", m_clock.nowMs());
        }
        return provider->fetchDeferred(prepared->model, handle, prepared->options);
    }

    Result<void> cancelDeferred(const Model& model, const DeferredHandle& handle, const StreamOptions& options) override {
        auto prepared = prepare(model, options);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        auto provider = m_providers.find(model.api);
        if (!provider || !provider->supportsDeferred()) {
            return std::unexpected(Error{"provider", "Provider " + model.provider + " does not support deferred responses"});
        }
        return provider->cancelDeferred(prepared->model, handle, prepared->options);
    }

    Result<void> registerProvider(const std::string& providerId, const Json& config) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto previous = m_registered.find(providerId);
        const std::optional<Json> backup =
            previous == m_registered.end() ? std::nullopt : std::optional<Json>(previous->second);
        m_registered[providerId] = config;
        const auto base = m_baseModels.find(providerId);
        const auto composed =
            m_composer.compose(providerId, base == m_baseModels.end() ? std::vector<Model>{} : base->second,
                               configFor(providerId));
        if (!composed) {
            if (backup) {
                m_registered[providerId] = *backup;
            } else {
                m_registered.erase(providerId);
            }
            return std::unexpected(composed.error());
        }
        buildProviders(m_baseModels, m_modelsJson);
        return {};
    }

    void unregisterProvider(const std::string& providerId) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_registered.erase(providerId);
        buildProviders(m_baseModels, m_modelsJson);
    }

    Result<void> registerApi(std::shared_ptr<IProvider> provider) override {
        const std::string api = provider->api();
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_providers.find(api) != nullptr) {
            return std::unexpected(Error{"provider", "The API \"" + api + "\" is already implemented"});
        }
        m_providers.registerProvider(std::move(provider));
        m_pluginApis.insert(api);
        return {};
    }

    void unregisterApi(const std::string& api) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pluginApis.erase(api) != 0) {
            m_providers.unregisterProvider(api);
        }
    }

    Result<void> registerOauthFlow(const std::string& provider, IOauthFlow& flow) override {
        return m_auth.addFlow(provider, flow);
    }

    void unregisterOauthFlow(const std::string& provider, const IOauthFlow& flow) override {
        m_auth.removeFlow(provider, flow);
    }

    Result<void> registerVirtualModel(VirtualModelDefinition definition) override {
        return m_virtual.add(std::move(definition), [this](const std::string& provider, const std::string& id) { return physicalModel(provider, id).has_value(); });
    }

    void unregisterVirtualModel(const std::string& provider, const std::string& id) override {
        m_virtual.remove(provider, id);
    }

    Result<VirtualRoute> resolveVirtual(const VirtualResolveRequest& request) override {
        return m_virtual.resolve(
            request, [this](const std::string& provider, const std::string& id) { return physicalModel(provider, id); },
            [this](const std::string& provider) { return hasConfiguredAuth(provider); });
    }

private:
    /** A physical model of the same provider and id as a virtual one is hidden by it. */
    bool hiddenByVirtual(const std::vector<Model>& virtualModels, const Model& model) const {
        return std::ranges::any_of(virtualModels, [&](const Model& entry) { return entry.provider == model.provider && entry.id == model.id; });
    }

    /** Resolves auth, headers and base URL for a request of `model`; the model's API provider is known to exist. */
    Result<PreparedRequest> prepare(const Model& model, const StreamOptions& options) {
        const auto found = state(model.provider);
        if (!found) {
            return std::unexpected(Error{"provider", "Unknown provider: " + model.provider});
        }
        auto resolution = resolveAuth(model.provider, options.apiKey, options.env);
        if (!resolution) {
            return std::unexpected(resolution.error());
        }
        if (!resolution->has_value()) {
            return std::unexpected(Error{"provider", "Provider is not configured: " + model.provider});
        }
        const AuthResult& auth = **resolution;
        if (!m_providers.find(model.api)) {
            return std::unexpected(Error{"provider", "No API provider registered for api: " + model.api});
        }
        HttpHeaders headers;
        if (!m_config.userAgent.empty()) {
            m_headers.set(headers, "User-Agent", m_config.userAgent);
        }
        for (const auto& [name, value] : auth.auth.headers) {
            m_headers.set(headers, name, value);
        }
        for (const auto& [name, value] : model.headers) {
            m_headers.set(headers, name, value);
        }
        std::map<std::string, std::string> env = auth.env;
        for (const auto& [name, value] : options.env) {
            env[name] = value;
        }
        for (const auto& [name, value] : configuredModelHeaders(model, found->config, env)) {
            m_headers.set(headers, name, value);
        }
        PreparedRequest prepared;
        prepared.options = options;
        prepared.options.apiKey = options.apiKey ? options.apiKey : auth.auth.apiKey;
        prepared.options.env = env;
        std::vector<std::pair<std::string, std::optional<std::string>>> merged;
        for (const auto& [name, value] : headers) {
            merged.emplace_back(name, value);
        }
        for (const auto& entry : options.headers) {
            merged.push_back(entry);
        }
        if (options.transformHeaders) {
            merged = options.transformHeaders(model, merged);
        }
        prepared.options.transformHeaders = nullptr;
        prepared.options.headers = std::move(merged);
        prepared.model = model;
        if (auth.auth.baseUrl) {
            prepared.model.baseUrl = *auth.auth.baseUrl;
        }
        return prepared;
    }

    using ModelMap = std::map<std::string, std::vector<Model>>;

    ModelMap loadBaseModels() {
        ModelMap base;
        if (m_config.catalogDir.empty()) {
            return base;
        }
        auto catalog = m_catalogLoader.load(m_config.catalogDir);
        if (!catalog) {
            setError("Failed to load model catalog: " + catalog.error().message);
            return base;
        }
        for (auto& [providerId, models] : *catalog) {
            base[providerId] = std::move(models);
        }
        return base;
    }

    void overlayRemote(ModelMap& base, std::int64_t generatedAtMs) {
        std::set<std::string> ids;
        for (const auto& definition : m_builtin.all()) {
            ids.insert(definition.id);
        }
        for (const auto& entry : base) {
            ids.insert(entry.first);
        }
        for (const auto& id : ids) {
            auto stored = m_store.read(id);
            if (!stored || !stored->has_value()) {
                continue;
            }
            const ModelsStoreEntry& entry = **stored;
            if (generatedAtMs > 0 && (!entry.lastModified || *entry.lastModified <= generatedAtMs)) {
                continue;
            }
            base[id] = mergeById(base[id], m_catalogLoader.parseModels(id, entry.models));
        }
    }

    std::vector<Model> mergeById(const std::vector<Model>& base, const std::vector<Model>& overlay) const {
        std::vector<Model> merged = base;
        for (const auto& model : overlay) {
            const auto found = std::find_if(merged.begin(), merged.end(),
                                            [&](const Model& entry) { return entry.id == model.id; });
            if (found == merged.end()) {
                merged.push_back(model);
            } else {
                *found = model;
            }
        }
        return merged;
    }

    ProviderDefinition definitionFor(const std::string& id, const Json& config) const {
        ProviderDefinition definition;
        if (auto builtin = m_builtin.find(id)) {
            definition = *builtin;
        } else {
            definition.id = id;
            definition.name = id;
            definition.envVars = m_envKeys.envVars(id);
            definition.builtin = false;
        }
        if (config.is_object()) {
            if (config.contains("name") && config["name"].is_string()) {
                definition.name = config["name"].get<std::string>();
            }
            if (config.contains("baseUrl") && config["baseUrl"].is_string()) {
                definition.baseUrl = config["baseUrl"].get<std::string>();
            }
            if (config.contains("oauth") && config["oauth"].is_string()) {
                definition.supportsOAuth = true;
            }
        }
        return definition;
    }

    Json configFor(const std::string& id) const {
        Json merged;
        if (m_modelsJson.is_object() && m_modelsJson.contains(id)) {
            merged = m_modelsJson[id];
        }
        const auto registered = m_registered.find(id);
        if (registered != m_registered.end()) {
            if (!merged.is_object()) {
                merged = Json::object();
            }
            for (const auto& entry : registered->second.items()) {
                const std::string& key = entry.key();
                const Json& value = entry.value();
                merged[key] = value;
            }
        }
        return merged;
    }

    void buildProviders(const ModelMap& base, const Json& modelsJson) {
        m_states.clear();
        std::set<std::string> ids;
        for (const auto& definition : m_builtin.all()) {
            ids.insert(definition.id);
        }
        for (const auto& entry : base) {
            ids.insert(entry.first);
        }
        if (modelsJson.is_object()) {
            for (const auto& entry : modelsJson.items()) {
                const std::string& id = entry.key();
                ids.insert(id);
            }
        }
        for (const auto& [id, section] : m_registered) {
            ids.insert(id);
        }
        for (const auto& id : ids) {
            const Json config = configFor(id);
            ProviderState state;
            state.definition = definitionFor(id, config);
            state.config = config;
            const auto found = base.find(id);
            const std::vector<Model> baseModels = found == base.end() ? std::vector<Model>{} : found->second;
            auto composed = m_composer.compose(id, baseModels, config);
            if (composed) {
                state.models = std::move(*composed);
            } else {
                setError(composed.error().message);
                state.models = baseModels;
            }
            m_states[id] = std::move(state);
        }
    }

    Result<std::optional<AuthResult>> resolveAuth(const std::string& provider, const std::optional<std::string>& apiKeyOverride, const std::map<std::string, std::string>& env) {
        const auto found = state(provider);
        if (!found) {
            return std::optional<AuthResult>();
        }
        std::optional<std::string> key = apiKeyOverride;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto runtime = m_runtimeKeys.find(provider);
            if (!key && runtime != m_runtimeKeys.end()) {
                key = runtime->second;
            }
        }
        return m_auth.resolve(found->definition, found->config, key, env);
    }

    HttpHeaders configuredModelHeaders(const Model& model, const Json& config, const std::map<std::string, std::string>& env) {
        ConfigValueResolver::Headers raw;
        const auto collect = [&](const Json& source) {
            if (source.is_object() && source.contains("headers") && source["headers"].is_object()) {
                for (const auto& entry : source["headers"].items()) {
                    const std::string& name = entry.key();
                    const Json& value = entry.value();
                    if (value.is_string()) {
                        raw[name] = value.get<std::string>();
                    }
                }
            }
        };
        if (config.is_object()) {
            if (config.contains("modelOverrides") && config["modelOverrides"].is_object() &&
                config["modelOverrides"].contains(model.id)) {
                collect(config["modelOverrides"][model.id]);
            }
            if (config.contains("models") && config["models"].is_array()) {
                for (const auto& definition : config["models"]) {
                    if (definition.is_object() && definition.value("id", "") == model.id) {
                        collect(definition);
                    }
                }
            }
        }
        HttpHeaders out;
        for (const auto& [name, value] : m_configValues.resolveHeaders(raw, env)) {
            out.emplace_back(name, value);
        }
        return out;
    }

    std::optional<ProviderState> state(const std::string& provider) const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_states.find(provider);
        if (found == m_states.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    void setError(std::string message) {
        if (!m_error) {
            m_error = std::move(message);
        } else {
            *m_error += "\n" + message;
        }
    }

    ModelRuntimeConfig m_config;
    ICredentialStore& m_credentials;
    IModelsStore& m_store;
    IProviderRegistry& m_providers;
    const EnvKeyTable& m_envKeys;
    ConfigValueResolver& m_configValues;
    const IClock& m_clock;
    ProviderAuthResolver m_auth;
    BuiltinProviderTable m_builtin;
    ModelCatalogLoader m_catalogLoader;
    ModelsConfigLoader m_modelsConfigLoader;
    ModelComposer m_composer;
    ErrorStreamFactory m_errors;
    HeaderMerger m_headers;
    VirtualModelRegistry m_virtual;

    mutable std::mutex m_mutex;
    std::map<std::string, ProviderState> m_states;
    std::map<std::string, Json> m_registered;
    /** APIs added through registerApi (plugins), the only ones unregisterApi removes. */
    std::set<std::string> m_pluginApis;
    std::map<std::string, std::string> m_runtimeKeys;
    Json m_modelsJson = Json::object();
    ModelMap m_baseModels;
    std::optional<std::string> m_error;
};
