export module pi.provider.i_model_runtime;

import std;
export import pi.provider.i_oauth_flow;
export import pi.provider.i_provider;
export import pi.types.auth_result;
export import pi.types.auth_status;
export import pi.types.deferred_handle;
export import pi.types.json;
export import pi.types.model;
export import pi.types.result;
export import pi.types.virtual_model_definition;
export import pi.types.virtual_resolve_request;
export import pi.types.virtual_route;

/**
 * The model catalog plus request-time authentication. Combines built-in providers, models.json
 * and plugin-registered providers; resolves credentials per request and routes streams to the
 * wire-API implementation of the model. Virtual models (api `pi-virtual`) are listed with the physical ones, but a provider
 * never receives them: a router picks the physical model of each request (resolveVirtual).
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
    /** The catalog model of that provider and id unless it is virtual. */
    virtual std::optional<Model> physicalModel(const std::string& provider, const std::string& id) const = 0;
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

    /**
     * Adds the wire-API implementation of `provider->api()` for models of plugin providers (their `api` names it). Fails when
     * that API is implemented already, so built-in APIs cannot be replaced and two plugins cannot share a name.
     */
    virtual Result<void> registerApi(std::shared_ptr<IProvider> provider) = 0;
    /** Removes an implementation added by registerApi; other APIs are left alone. */
    virtual void unregisterApi(const std::string& api) = 0;

    /**
     * Adds the OAuth flow (refresh and API key derivation) of a plugin provider; the flow must outlive its registration. Fails
     * when the provider already has a flow, so built-in sign-ins cannot be replaced.
     */
    virtual Result<void> registerOauthFlow(const std::string& provider, IOauthFlow& flow) = 0;
    /** Removes a flow added by registerOauthFlow, if `flow` is still the provider's. */
    virtual void unregisterOauthFlow(const std::string& provider, const IOauthFlow& flow) = 0;

    /**
     * Registers or replaces a virtual model under `definition.provider`, which may also list physical models. Fails when the id
     * is a physical model of that provider. A provider with only virtual models needs no credentials.
     */
    virtual Result<void> registerVirtualModel(VirtualModelDefinition definition) = 0;
    virtual void unregisterVirtualModel(const std::string& provider, const std::string& id) = 0;

    /**
     * Asks the router of a virtual model for the physical model and thinking level of one request; fails when the model is not
     * registered, the router fails or it answers with a virtual model or a model without credentials. The thinking level is
     * clamped to the answer.
     */
    virtual Result<VirtualRoute> resolveVirtual(const VirtualResolveRequest& request) = 0;
};
