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

    void addModel(const Model& model);
    void setAuthenticated(const std::string& provider, bool authenticated);
    void setStreamHandler(StreamHandler handler);

    Result<void> reload() override;
    std::optional<std::string> error() const override;
    std::vector<Model> models() const override;
    std::vector<Model> availableModels() override;
    std::optional<Model> find(const std::string& provider, const std::string& id) const override;
    std::string providerName(const std::string& provider) const override;
    std::vector<std::string> providerIds() const override;
    AuthStatus authStatus(const std::string& provider) override;
    bool hasConfiguredAuth(const std::string& provider) override;
    void setRuntimeApiKey(const std::string& provider, const std::string& apiKey) override;
    void removeRuntimeApiKey(const std::string& provider) override;
    Result<std::optional<AuthResult>> getAuth(const std::string& provider,
                                              const std::optional<std::string>& apiKeyOverride,
                                              const std::map<std::string, std::string>& env) override;
    std::shared_ptr<AssistantMessageStream> stream(const Model& model, const TranscriptContext& context,
                                                   const StreamOptions& options) override;
    Result<void> registerProvider(const std::string& providerId, const Json& config) override;
    void unregisterProvider(const std::string& providerId) override;

private:
    mutable std::mutex m_mutex;
    std::vector<Model> m_models;
    std::set<std::string> m_authenticated;
    StreamHandler m_handler;
};

void FakeModelRuntime::addModel(const Model& model) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_models.push_back(model);
}

void FakeModelRuntime::setAuthenticated(const std::string& provider, bool authenticated) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (authenticated) {
        m_authenticated.insert(provider);
    } else {
        m_authenticated.erase(provider);
    }
}

void FakeModelRuntime::setStreamHandler(StreamHandler handler) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_handler = std::move(handler);
}

Result<void> FakeModelRuntime::reload() {
    return {};
}

std::optional<std::string> FakeModelRuntime::error() const {
    return std::nullopt;
}

std::vector<Model> FakeModelRuntime::models() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_models;
}

std::vector<Model> FakeModelRuntime::availableModels() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Model> out;
    for (const auto& model : m_models) {
        if (m_authenticated.contains(model.provider)) {
            out.push_back(model);
        }
    }
    return out;
}

std::optional<Model> FakeModelRuntime::find(const std::string& provider, const std::string& id) const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& model : m_models) {
        if (model.provider == provider && model.id == id) {
            return model;
        }
    }
    return std::nullopt;
}

std::string FakeModelRuntime::providerName(const std::string& provider) const {
    return provider;
}

std::vector<std::string> FakeModelRuntime::providerIds() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::set<std::string> ids;
    for (const auto& model : m_models) {
        ids.insert(model.provider);
    }
    return {ids.begin(), ids.end()};
}

AuthStatus FakeModelRuntime::authStatus(const std::string& provider) {
    AuthStatus status;
    status.configured = hasConfiguredAuth(provider);
    status.source = status.configured ? "stored" : "";
    return status;
}

bool FakeModelRuntime::hasConfiguredAuth(const std::string& provider) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_authenticated.contains(provider);
}

void FakeModelRuntime::setRuntimeApiKey(const std::string& provider, const std::string&) {
    setAuthenticated(provider, true);
}

void FakeModelRuntime::removeRuntimeApiKey(const std::string& provider) {
    setAuthenticated(provider, false);
}

Result<std::optional<AuthResult>> FakeModelRuntime::getAuth(const std::string& provider,
                                                            const std::optional<std::string>&,
                                                            const std::map<std::string, std::string>&) {
    if (!hasConfiguredAuth(provider)) {
        return std::optional<AuthResult>();
    }
    AuthResult result;
    result.auth.apiKey = "key";
    return std::optional<AuthResult>(result);
}

std::shared_ptr<AssistantMessageStream> FakeModelRuntime::stream(const Model& model,
                                                                 const TranscriptContext& context,
                                                                 const StreamOptions& options) {
    StreamHandler handler;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        handler = m_handler;
    }
    return handler ? handler(model, context, options) : nullptr;
}

Result<void> FakeModelRuntime::registerProvider(const std::string&, const Json&) {
    return {};
}

void FakeModelRuntime::unregisterProvider(const std::string&) {}
