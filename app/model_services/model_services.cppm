module;

#include <nlohmann/json.hpp>

export module pi.model_services;

import std;
export import pi.provider.i_model_runtime;
import pi.ai.anthropic_messages_provider;
import pi.ai.azure_responses_provider;
import pi.ai.bedrock_provider;
import pi.ai.chat_completions_provider;
import pi.ai.faux_provider;
import pi.ai.google_adc_auth;
import pi.ai.google_provider;
import pi.ai.google_vertex_provider;
import pi.ai.codex_provider;
import pi.ai.file_credential_store;
import pi.ai.file_models_store;
import pi.ai.mistral_provider;
import pi.ai.model_runtime;
import pi.ai.pi_messages_provider;
import pi.ai.provider_registry;
import pi.ai.responses_provider;
import pi.platform_services;

/**
 * The model side of the application: credential and models.json stores under the agent directory,
 * the wire-API providers and the model runtime that combines them. With `faux` enabled a scripted
 * provider and model ("faux/faux-1") are added for offline runs; PI_FAUX_REPLIES (a JSON array of
 * strings) scripts its replies.
 */
export class ModelServices {
public:
    ModelServices(PlatformServices& platform, const std::string& agentDir, const std::string& catalogDir, bool faux);

    ModelRuntime& models();
    ConfigValueResolver& configValues();
    FauxProvider* faux();

private:
    void registerFaux(const std::string& replies);

    PlatformServices& m_platform;
    ConfigValueResolver m_configValues;
    FileCredentialStore m_credentials;
    FileModelsStore m_modelsStore;
    EnvKeyTable m_envKeys;
    GoogleAdcAuth m_adc;
    ProviderRegistry m_providers;
    std::shared_ptr<FauxProvider> m_faux;
    ModelRuntime m_runtime;
};

ModelServices::ModelServices(PlatformServices& platform, const std::string& agentDir, const std::string& catalogDir,
                             bool faux)
    : m_platform(platform),
      m_configValues(platform.environment(), platform.processes()),
      m_credentials(agentDir + "/auth.json", platform.files(), platform.locks(), m_configValues),
      m_modelsStore(agentDir + "/models-cache.json", platform.files(), platform.locks()),
      m_envKeys(platform.environment(), platform.files()),
      m_adc(platform.http(), platform.files(), platform.environment(), platform.clock(), platform.crypto(),
            platform.base64()),
      m_runtime(ModelRuntimeConfig{agentDir + "/models.json", catalogDir}, m_credentials, m_modelsStore,
                platform.files(), m_providers, m_envKeys, m_configValues, platform.clock(), {}) {
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

void ModelServices::registerFaux(const std::string& replies) {
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

ModelRuntime& ModelServices::models() {
    return m_runtime;
}

ConfigValueResolver& ModelServices::configValues() {
    return m_configValues;
}

FauxProvider* ModelServices::faux() {
    return m_faux.get();
}
