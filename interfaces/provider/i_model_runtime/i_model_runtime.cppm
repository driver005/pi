export module pi.provider.i_model_runtime;

import std;
export import pi.provider.i_provider;
export import pi.types.auth_result;
export import pi.types.auth_status;
export import pi.types.deferred_handle;
export import pi.types.json;
export import pi.types.model;
export import pi.types.result;

/**
 * The model catalog plus request-time authentication. Combines built-in providers, models.json
 * and plugin-registered providers; resolves credentials per request and routes streams to the
 * wire-API implementation of the model.
 */
export class IModelRuntime {
public:
    virtual ~IModelRuntime() = default;

    /** Re-reads catalogs and models.json. A bad models.json is reported by error(). */
    virtual Result<void> reload() = 0;
    virtual std::optional<std::string> error() const = 0;

    virtual std::vector<Model> models() const = 0;
    /** Models whose provider has usable credentials. */
    virtual std::vector<Model> availableModels() = 0;
    virtual std::optional<Model> find(const std::string& provider, const std::string& id) const = 0;
    virtual std::string providerName(const std::string& provider) const = 0;
    virtual std::vector<std::string> providerIds() const = 0;

    virtual AuthStatus authStatus(const std::string& provider) = 0;
    virtual bool hasConfiguredAuth(const std::string& provider) = 0;
    /** Session-only API key that takes precedence over stored credentials. */
    virtual void setRuntimeApiKey(const std::string& provider, const std::string& apiKey) = 0;
    virtual void removeRuntimeApiKey(const std::string& provider) = 0;

    /** nullopt: the provider has no credentials. Error: credentials exist but cannot be used. */
    virtual Result<std::optional<AuthResult>> getAuth(
        const std::string& provider, const std::optional<std::string>& apiKeyOverride = std::nullopt,
        const std::map<std::string, std::string>& env = {}) = 0;

    /**
     * Resolves auth (this may block on an OAuth refresh) and streams through the wire API of the
     * model. Failures arrive as an Error event; this never fails synchronously.
     */
    virtual std::shared_ptr<AssistantMessageStream> stream(const Model& model,
                                                           const TranscriptContext& context,
                                                           const StreamOptions& options) = 0;

    /**
     * Fetches the current state of a deferred response (another `deferred` message while pending, then the final one),
     * with the same auth resolution as stream(). Failures, including a provider without deferred support, arrive as an
     * Error event.
     */
    virtual std::shared_ptr<AssistantMessageStream> fetchDeferred(const Model& model, const DeferredHandle& handle,
                                                                  const StreamOptions& options) = 0;

    /** Cancels a deferred response. */
    virtual Result<void> cancelDeferred(const Model& model, const DeferredHandle& handle, const StreamOptions& options) = 0;

    /** Registers or replaces a provider from a models.json-style config (plugins). */
    virtual Result<void> registerProvider(const std::string& providerId, const Json& config) = 0;
    virtual void unregisterProvider(const std::string& providerId) = 0;
};
