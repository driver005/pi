export module pi.model_services;

import std;
import pi.support.pi_user_agent;
export import pi.provider.i_model_runtime;
import pi.ai.anthropic_messages_provider;
import pi.ai.azure_responses_provider;
import pi.ai.bedrock_provider;
import pi.ai.chat_completions_provider;
import pi.ai.faux_provider;
import pi.ai.github_copilot_oauth_flow;
import pi.ai.meta_oauth_flow;
import pi.ai.google_adc_auth;
import pi.ai.google_provider;
import pi.ai.google_vertex_provider;
import pi.ai.codex_provider;
import pi.ai.file_credential_store;
import pi.ai.file_models_store;
import pi.ai.mistral_provider;
import pi.ai.model_runtime;
import pi.ai.oauth_refresh_flow;
import pi.ai.pi_messages_provider;
import pi.ai.provider_registry;
import pi.ai.responses_provider;
import pi.platform_services;
import pi.support.builtin_oauth_specs;
import pi.support.radius_catalog;
import pi.support.radius_gateway;

/**
 * The model side of the application: credential and models.json stores under the agent directory,
 * the wire-API providers and the model runtime that combines them. With `faux` enabled a scripted
 * provider and model ("faux/faux-1") are added for offline runs; PI_FAUX_REPLIES (a JSON array of
 * strings) scripts its replies.
 */
export class ModelServices {
public:
    ModelServices(PlatformServices& platform, const std::string& agentDir, const std::string& catalogDir, bool faux)
        : m_platform(platform),
          m_configValues(platform.environment(), platform.processes()),
          m_credentials(agentDir + "/auth.json", platform.files(), platform.locks(), m_configValues),
          m_modelsStore(agentDir + "/models-cache.json", platform.files(), platform.locks()),
          m_radiusCatalog(platform.http(), m_modelsStore, platform.clock()),
          m_envKeys(platform.environment(), platform.files()),
          m_adc(platform.http(), platform.files(), platform.environment(), platform.clock(), platform.crypto(), platform.base64()),
          m_flows(buildFlows()),
          m_copilot(platform.http()),
          m_meta(platform.http(), platform.clock()),
          m_runtime(ModelRuntimeConfig{agentDir + "/models.json", catalogDir, PiUserAgent().value(platform.system()), RadiusGateway().gatewayUrl(platform.environment())}, m_credentials, m_modelsStore, platform.files(), m_providers, m_envKeys, m_configValues, platform.clock(), flowMap()) {
        m_providers.registerProvider(std::make_shared<AnthropicMessagesProvider>(
            platform.http(), platform.sleeper(), platform.clock(), platform.executor()));
        m_providers.registerProvider(std::make_shared<ChatCompletionsProvider>(
            platform.http(), platform.sleeper(), platform.clock(), platform.executor()));
        m_providers.registerProvider(std::make_shared<ResponsesProvider>(
            platform.http(), platform.sleeper(), platform.clock(), platform.executor()));
        m_providers.registerProvider(std::make_shared<AzureResponsesProvider>(
            platform.http(), platform.sleeper(), platform.clock(), platform.executor(), platform.environment()));
        m_providers.registerProvider(std::make_shared<GoogleProvider>(
            platform.http(), platform.sleeper(), platform.clock(), platform.executor()));
        m_providers.registerProvider(std::make_shared<GoogleVertexProvider>(
            platform.http(), platform.sleeper(), platform.clock(), platform.executor(), platform.environment(), m_adc));
        m_providers.registerProvider(std::make_shared<MistralProvider>(
            platform.http(), platform.sleeper(), platform.clock(), platform.executor()));
        m_providers.registerProvider(std::make_shared<PiMessagesProvider>(
            platform.http(), platform.sleeper(), platform.clock(), platform.executor()));
        m_providers.registerProvider(std::make_shared<CodexProvider>(
            platform.http(), platform.sleeper(), platform.clock(), platform.executor(), platform.base64()));
        m_providers.registerProvider(std::make_shared<BedrockProvider>(
            platform.http(), platform.sleeper(), platform.clock(), platform.executor(), platform.environment(),
            platform.files(), platform.crypto(), platform.base64()));
        if (faux) {
            const auto replies = platform.environment().get("PI_FAUX_REPLIES");
            registerFaux(replies.value_or("[]"));
        }
        m_runtime.reload();
    }

    ModelRuntime& models() {
        return m_runtime;
    }

    ConfigValueResolver& configValues() {
        return m_configValues;
    }

    FauxProvider* faux() {
        return m_faux.get();
    }

    /** The credential store under the agent directory (`auth.json`). */
    ICredentialStore& credentials() {
        return m_credentials;
    }

    /** The OAuth flows by provider id: how stored credentials of subscription providers are refreshed. */
    std::map<std::string, IOauthFlow*> flows() {
        return flowMap();
    }

    /** The Radius gateway origin: PI_RADIUS_GATEWAY, else the default gateway. */
    std::string radiusGateway() const {
        return RadiusGateway().gatewayUrl(m_platform.environment());
    }

    /**
     * Fetches the Radius gateway's model catalog (signed in with the stored Radius credential or `RADIUS_API_KEY` when there
     * is one), keeps it in the models store and has the runtime list it. Does nothing while `PI_OFFLINE` is set. A failure
     * keeps the stored catalog and is returned; callers report it and carry on.
     */
    Result<void> refreshRadiusCatalog(const std::shared_ptr<AbortSignal>& signal = nullptr) {
        if (m_platform.environment().get("PI_OFFLINE")) {
            return {};
        }
        std::optional<std::string> key;
        if (const auto auth = m_runtime.getAuth(RadiusGateway().providerId(), std::nullopt, {}); auth && *auth) {
            key = (*auth)->auth.apiKey;
        }
        if (auto refreshed = m_radiusCatalog.refresh(radiusGateway(), key, signal); !refreshed) {
            return refreshed;
        }
        return m_runtime.reload();
    }

    /** The Kimi OAuth host override from the environment (KIMI_CODE_OAUTH_HOST or KIMI_OAUTH_HOST); empty for the default. */
    std::string kimiHost() const {
        std::string host = m_platform.environment().get("KIMI_CODE_OAUTH_HOST").value_or("");
        return host.empty() ? m_platform.environment().get("KIMI_OAUTH_HOST").value_or("") : host;
    }

private:
    std::vector<std::unique_ptr<OauthRefreshFlow>> buildFlows() const {
        std::vector<std::unique_ptr<OauthRefreshFlow>> flows;
        for (auto& spec : BuiltinOauthSpecs().all(kimiHost())) {
            flows.push_back(std::make_unique<OauthRefreshFlow>(std::move(spec), m_platform.http(), m_platform.clock(),
                                                               m_platform.base64()));
        }
        flows.push_back(std::make_unique<OauthRefreshFlow>(BuiltinOauthSpecs().radius(radiusGateway()), m_platform.http(), m_platform.clock(), m_platform.base64()));
        return flows;
    }

    std::map<std::string, IOauthFlow*> flowMap() {
        std::map<std::string, IOauthFlow*> flows;
        for (const auto& flow : m_flows) {
            flows[flow->providerId()] = flow.get();
        }
        flows["github-copilot"] = &m_copilot;
        flows["meta"] = &m_meta;
        return flows;
    }

    void registerFaux(const std::string& replies) {
        m_faux = std::make_shared<FauxProvider>(m_platform.executor(), m_platform.clock(), "faux");
        const Json parsed = Json::parse(replies, nullptr, false);
        if (parsed.is_array()) {
            for (const auto& reply : parsed) {
                if (reply.is_string()) {
                    m_faux->enqueue(m_faux->textResponse(reply.get<std::string>()));
                }
            }
        }
        m_providers.registerProvider(m_faux);
        Json config = Json::object();
        config["baseUrl"] = "http://localhost/faux";
        config["api"] = "faux";
        config["apiKey"] = "faux";
        config["models"] = Json::array({Json{{"id", "faux-1"}, {"name", "Faux"}, {"contextWindow", 100000}, {"maxTokens", 8000}}});
        m_runtime.registerProvider("faux", config);
    }

    PlatformServices& m_platform;
    ConfigValueResolver m_configValues;
    FileCredentialStore m_credentials;
    FileModelsStore m_modelsStore;
    RadiusCatalog m_radiusCatalog;
    EnvKeyTable m_envKeys;
    GoogleAdcAuth m_adc;
    std::vector<std::unique_ptr<OauthRefreshFlow>> m_flows;
    GithubCopilotOauthFlow m_copilot;
    MetaOauthFlow m_meta;
    ProviderRegistry m_providers;
    std::shared_ptr<FauxProvider> m_faux;
    ModelRuntime m_runtime;
};
