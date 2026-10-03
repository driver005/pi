export module pi.testing.fake_model_runtime;

import std;
export import pi.provider.i_model_runtime;

/**
 * IModelRuntime with a fixed model list and a set of providers that have credentials. Streaming
 * is delegated to a handler so tests can route it to a faux provider.
 */
export class FakeModelRuntime : public IModelRuntime {
public:
    using StreamHandler = std::function<std::shared_ptr<AssistantMessageStream>(
        const Model&, const TranscriptContext&, const StreamOptions&)>;

    void addModel(const Model& model) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_models.push_back(model);
    }

    void setAuthenticated(const std::string& provider, bool authenticated) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (authenticated) {
            m_authenticated.insert(provider);
        } else {
            m_authenticated.erase(provider);
        }
    }

    using FetchDeferredHandler = std::function<std::shared_ptr<AssistantMessageStream>(const Model&, const DeferredHandle&, const StreamOptions&)>;
    using CancelDeferredHandler = std::function<Result<void>(const Model&, const DeferredHandle&, const StreamOptions&)>;

    void setDeferredHandlers(FetchDeferredHandler fetch, CancelDeferredHandler cancel) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_fetchDeferred = std::move(fetch);
        m_cancelDeferred = std::move(cancel);
    }

    void setStreamHandler(StreamHandler handler) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_handler = std::move(handler);
    }

    Result<void> reload() override {
        return {};
    }

    std::optional<std::string> error() const override {
        return std::nullopt;
    }

    std::vector<Model> models() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_models;
    }

    std::vector<Model> availableModels() override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Model> out;
        for (const auto& model : m_models) {
            if (m_authenticated.contains(model.provider)) {
                out.push_back(model);
            }
        }
        return out;
    }

    std::optional<Model> find(const std::string& provider, const std::string& id) const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& model : m_models) {
            if (model.provider == provider && model.id == id) {
                return model;
            }
        }
        return std::nullopt;
    }

    std::string providerName(const std::string& provider) const override {
        return provider;
    }

    std::vector<std::string> providerIds() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::set<std::string> ids;
        for (const auto& model : m_models) {
            ids.insert(model.provider);
        }
        return {ids.begin(), ids.end()};
    }

    AuthStatus authStatus(const std::string& provider) override {
        AuthStatus status;
        status.configured = hasConfiguredAuth(provider);
        status.source = status.configured ? "stored" : "";
        return status;
    }

    bool hasConfiguredAuth(const std::string& provider) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_authenticated.contains(provider);
    }

    void setRuntimeApiKey(const std::string& provider, const std::string&) override {
        setAuthenticated(provider, true);
    }

    void removeRuntimeApiKey(const std::string& provider) override {
        setAuthenticated(provider, false);
    }

    Result<std::optional<AuthResult>> getAuth(const std::string& provider, const std::optional<std::string>&, const std::map<std::string, std::string>&) override {
        if (!hasConfiguredAuth(provider)) {
            return std::optional<AuthResult>();
        }
        AuthResult result;
        result.auth.apiKey = "key";
        return std::optional<AuthResult>(result);
    }

    std::shared_ptr<AssistantMessageStream> stream(const Model& model, const TranscriptContext& context, const StreamOptions& options) override {
        StreamHandler handler;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            handler = m_handler;
        }
        return handler ? handler(model, context, options) : nullptr;
    }

    std::shared_ptr<AssistantMessageStream> fetchDeferred(const Model& model, const DeferredHandle& handle, const StreamOptions& options) override {
        FetchDeferredHandler handler;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            handler = m_fetchDeferred;
        }
        return handler ? handler(model, handle, options) : nullptr;
    }

    Result<void> cancelDeferred(const Model& model, const DeferredHandle& handle, const StreamOptions& options) override {
        CancelDeferredHandler handler;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            handler = m_cancelDeferred;
        }
        if (!handler) {
            return std::unexpected(Error{"provider", "No deferred handler is set"});
        }
        return handler(model, handle, options);
    }

    Result<void> registerProvider(const std::string& providerId, const Json& config) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_rejectProviders) {
            return std::unexpected(Error{"provider", "rejected by the test"});
        }
        m_registered[providerId] = config;
        return {};
    }
    void unregisterProvider(const std::string& providerId) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_registered.erase(providerId);
    }

    /** Providers registered through registerProvider and not unregistered since, by id. */
    std::map<std::string, Json> registeredProviders() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_registered;
    }

    /** Makes registerProvider fail. */
    void rejectProviders() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_rejectProviders = true;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<Model> m_models;
    std::set<std::string> m_authenticated;
    std::map<std::string, Json> m_registered;
    bool m_rejectProviders = false;
    StreamHandler m_handler;
    FetchDeferredHandler m_fetchDeferred;
    CancelDeferredHandler m_cancelDeferred;
};
