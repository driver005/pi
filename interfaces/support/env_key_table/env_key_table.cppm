export module pi.support.env_key_table;

import std;
export import pi.platform.i_environment;
export import pi.platform.i_file_system;

/**
 * Which environment variables hold a provider's API key, and which ambient credentials make a
 * provider usable without one (Vertex ADC, AWS). Port of packages/ai/src/env-api-keys.ts.
 */
export class EnvKeyTable {
public:
    using Overrides = std::map<std::string, std::string>;

    EnvKeyTable(const IEnvironment& environment, IFileSystem& files);

    /** Variables that can provide the key, in priority order; empty for unknown providers. */
    std::vector<std::string> envVars(const std::string& provider) const;

    /** The subset of envVars that is currently set. */
    std::vector<std::string> findEnvKeys(const std::string& provider, const Overrides& env) const;

    /**
     * Key from the first usable variable (ANTHROPIC_AUTH_TOKEN is skipped: it must be sent as a
     * Bearer header). Vertex and Bedrock return "<authenticated>" when ambient credentials exist.
     */
    std::optional<std::string> apiKey(const std::string& provider, const Overrides& env) const;

    /** Environment value, with explicit overrides first; blank counts as unset. */
    std::optional<std::string> value(const std::string& name, const Overrides& env) const;

private:
    bool hasVertexAdc(const Overrides& env) const;
    bool hasBedrockCredentials(const Overrides& env) const;

    const IEnvironment& m_environment;
    IFileSystem& m_files;
};

EnvKeyTable::EnvKeyTable(const IEnvironment& environment, IFileSystem& files)
    : m_environment(environment), m_files(files) {}

std::vector<std::string> EnvKeyTable::envVars(const std::string& provider) const {
    if (provider == "github-copilot") {
        return {"COPILOT_GITHUB_TOKEN"};
    }
    if (provider == "anthropic") {
        return {"ANTHROPIC_AUTH_TOKEN", "ANTHROPIC_OAUTH_TOKEN", "ANTHROPIC_API_KEY"};
    }
    const std::map<std::string, std::string> table = {
        {"ant-ling", "ANT_LING_API_KEY"},
        {"qwen-token-plan", "QWEN_TOKEN_PLAN_API_KEY"},
        {"qwen-token-plan-cn", "QWEN_TOKEN_PLAN_CN_API_KEY"},
        {"qwen-token-plan-individual", "QWEN_TOKEN_PLAN_API_KEY"},
        {"openai", "OPENAI_API_KEY"},
        {"azure-openai-responses", "AZURE_OPENAI_API_KEY"},
        {"nvidia", "NVIDIA_API_KEY"},
        {"deepseek", "DEEPSEEK_API_KEY"},
        {"google", "GEMINI_API_KEY"},
        {"google-vertex", "GOOGLE_CLOUD_API_KEY"},
        {"groq", "GROQ_API_KEY"},
        {"cerebras", "CEREBRAS_API_KEY"},
        {"xai", "XAI_API_KEY"},
        {"typesafe", "TYPESAFE_API_KEY"},
        {"radius", "RADIUS_API_KEY"},
        {"openrouter", "OPENROUTER_API_KEY"},
        {"vercel-ai-gateway", "AI_GATEWAY_API_KEY"},
        {"zai", "ZAI_API_KEY"},
        {"zai-coding-cn", "ZAI_CODING_CN_API_KEY"},
        {"mistral", "MISTRAL_API_KEY"},
        {"minimax", "MINIMAX_API_KEY"},
        {"minimax-cn", "MINIMAX_CN_API_KEY"},
        {"moonshotai", "MOONSHOT_API_KEY"},
        {"moonshotai-cn", "MOONSHOT_API_KEY"},
        {"huggingface", "HF_TOKEN"},
        {"fireworks", "FIREWORKS_API_KEY"},
        {"together", "TOGETHER_API_KEY"},
        {"baseten", "BASETEN_API_KEY"},
        {"opencode", "OPENCODE_API_KEY"},
        {"opencode-go", "OPENCODE_API_KEY"},
        {"kimi-coding", "KIMI_API_KEY"},
        {"meta", "META_API_KEY"},
        {"cloudflare-workers-ai", "CLOUDFLARE_API_KEY"},
        {"cloudflare-ai-gateway", "CLOUDFLARE_API_KEY"},
        {"xiaomi", "XIAOMI_API_KEY"},
        {"xiaomi-token-plan-cn", "XIAOMI_TOKEN_PLAN_CN_API_KEY"},
        {"xiaomi-token-plan-ams", "XIAOMI_TOKEN_PLAN_AMS_API_KEY"},
        {"xiaomi-token-plan-sgp", "XIAOMI_TOKEN_PLAN_SGP_API_KEY"}};
    const auto found = table.find(provider);
    if (found == table.end()) {
        return {};
    }
    return {found->second};
}

std::optional<std::string> EnvKeyTable::value(const std::string& name, const Overrides& env) const {
    const auto explicitValue = env.find(name);
    if (explicitValue != env.end() && explicitValue->second.find_first_not_of(" \t\r\n") != std::string::npos) {
        return explicitValue->second;
    }
    const auto ambient = m_environment.get(name);
    if (ambient && ambient->find_first_not_of(" \t\r\n") != std::string::npos) {
        return ambient;
    }
    return std::nullopt;
}

std::vector<std::string> EnvKeyTable::findEnvKeys(const std::string& provider,
                                                  const Overrides& env) const {
    std::vector<std::string> found;
    for (const auto& name : envVars(provider)) {
        if (value(name, env)) {
            found.push_back(name);
        }
    }
    return found;
}

bool EnvKeyTable::hasVertexAdc(const Overrides& env) const {
    if (const auto explicitPath = env.find("GOOGLE_APPLICATION_CREDENTIALS");
        explicitPath != env.end() && !explicitPath->second.empty()) {
        return m_files.exists(explicitPath->second);
    }
    if (const auto path = value("GOOGLE_APPLICATION_CREDENTIALS", env)) {
        return m_files.exists(*path);
    }
    return m_files.exists(m_files.homeDirectory() +
                          "/.config/gcloud/application_default_credentials.json");
}

bool EnvKeyTable::hasBedrockCredentials(const Overrides& env) const {
    return value("AWS_PROFILE", env) ||
           (value("AWS_ACCESS_KEY_ID", env) && value("AWS_SECRET_ACCESS_KEY", env)) ||
           value("AWS_BEARER_TOKEN_BEDROCK", env) ||
           value("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI", env) ||
           value("AWS_CONTAINER_CREDENTIALS_FULL_URI", env) || value("AWS_WEB_IDENTITY_TOKEN_FILE", env);
}

std::optional<std::string> EnvKeyTable::apiKey(const std::string& provider,
                                               const Overrides& env) const {
    const auto keys = findEnvKeys(provider, env);
    for (const auto& name : keys) {
        if (provider == "anthropic" && name == "ANTHROPIC_AUTH_TOKEN") {
            continue;
        }
        return value(name, env);
    }
    if (provider == "google-vertex") {
        const bool project = value("GOOGLE_CLOUD_PROJECT", env) || value("GCLOUD_PROJECT", env);
        if (hasVertexAdc(env) && project && value("GOOGLE_CLOUD_LOCATION", env)) {
            return "<authenticated>";
        }
    }
    if (provider == "amazon-bedrock" && hasBedrockCredentials(env)) {
        return "<authenticated>";
    }
    return std::nullopt;
}
