export module pi.support.builtin_provider_table;

import std;
export import pi.types.provider_definition;

/**
 * The built-in model providers and how their credentials are found. Models themselves come from
 * the generated catalog; this table covers identity and auth. Mirrors the provider files under packages/ai/src/providers.
 */
export class BuiltinProviderTable {
public:
    std::vector<ProviderDefinition> all() const;
    std::optional<ProviderDefinition> find(const std::string& id) const;
};

std::vector<ProviderDefinition> BuiltinProviderTable::all() const {
    return {
        ProviderDefinition{"amazon-bedrock", "Amazon Bedrock", "", {}, true, false, true},
        ProviderDefinition{"ant-ling", "Ant Ling", "https://api.ant-ling.com/v1", {"ANT_LING_API_KEY"}, true, false, true},
        ProviderDefinition{"anthropic", "Anthropic", "https://api.anthropic.com", {"ANTHROPIC_AUTH_TOKEN", "ANTHROPIC_OAUTH_TOKEN", "ANTHROPIC_API_KEY"}, true, true, true},
        ProviderDefinition{"azure-openai-responses", "Azure OpenAI", "", {"AZURE_OPENAI_API_KEY"}, true, false, true},
        ProviderDefinition{"baseten", "Baseten", "https://inference.baseten.co/v1", {"BASETEN_API_KEY"}, true, false, true},
        ProviderDefinition{"cerebras", "Cerebras", "https://api.cerebras.ai/v1", {"CEREBRAS_API_KEY"}, true, false, true},
        ProviderDefinition{"cloudflare-ai-gateway", "Cloudflare AI Gateway", "", {"CLOUDFLARE_API_KEY"}, true, false, true},
        ProviderDefinition{"cloudflare-workers-ai", "Cloudflare Workers AI", "", {"CLOUDFLARE_API_KEY"}, true, false, true},
        ProviderDefinition{"deepseek", "DeepSeek", "https://api.deepseek.com", {"DEEPSEEK_API_KEY"}, true, false, true},
        ProviderDefinition{"fireworks", "Fireworks", "https://api.fireworks.ai/inference", {"FIREWORKS_API_KEY"}, true, false, true},
        ProviderDefinition{"github-copilot", "GitHub Copilot", "https://api.individual.githubcopilot.com", {"COPILOT_GITHUB_TOKEN"}, true, true, true},
        ProviderDefinition{"google-vertex", "Google Vertex AI", "", {"GOOGLE_CLOUD_API_KEY"}, true, false, true},
        ProviderDefinition{"google", "Google", "https://generativelanguage.googleapis.com/v1beta", {"GEMINI_API_KEY"}, true, false, true},
        ProviderDefinition{"groq", "Groq", "https://api.groq.com/openai/v1", {"GROQ_API_KEY"}, true, false, true},
        ProviderDefinition{"huggingface", "Hugging Face", "https://router.huggingface.co/v1", {"HF_TOKEN"}, true, false, true},
        ProviderDefinition{"kimi-coding", "Kimi For Coding", "https://api.kimi.com/coding", {"KIMI_API_KEY"}, true, true, true},
        ProviderDefinition{"meta", "Meta", "https://api.meta.ai/v1", {"META_API_KEY"}, true, true, true},
        ProviderDefinition{"minimax-cn", "MiniMax CN", "https://api.minimaxi.com/anthropic", {"MINIMAX_CN_API_KEY"}, true, false, true},
        ProviderDefinition{"minimax", "MiniMax", "https://api.minimax.io/anthropic", {"MINIMAX_API_KEY"}, true, false, true},
        ProviderDefinition{"mistral", "Mistral", "https://api.mistral.ai", {"MISTRAL_API_KEY"}, true, false, true},
        ProviderDefinition{"moonshotai-cn", "Moonshot AI CN", "https://api.moonshot.cn/v1", {"MOONSHOT_API_KEY"}, true, false, true},
        ProviderDefinition{"moonshotai", "Moonshot AI", "https://api.moonshot.ai/v1", {"MOONSHOT_API_KEY"}, true, false, true},
        ProviderDefinition{"nvidia", "NVIDIA", "https://integrate.api.nvidia.com/v1", {"NVIDIA_API_KEY"}, true, false, true},
        ProviderDefinition{"openai-codex", "OpenAI Codex (legacy)", "https://chatgpt.com/backend-api", {}, false, true, true},
        ProviderDefinition{"openai", "OpenAI", "https://api.openai.com/v1", {"OPENAI_API_KEY"}, true, true, true},
        ProviderDefinition{"opencode-go", "OpenCode Go", "", {"OPENCODE_API_KEY"}, true, false, true},
        ProviderDefinition{"opencode", "OpenCode Zen", "", {"OPENCODE_API_KEY"}, true, false, true},
        ProviderDefinition{"openrouter", "OpenRouter", "https://openrouter.ai/api/v1", {"OPENROUTER_API_KEY"}, true, true, true},
        ProviderDefinition{"qwen-token-plan-cn", "Qwen Token Plan CN", "https://token-plan.cn-beijing.maas.aliyuncs.com/compatible-mode/v1", {"QWEN_TOKEN_PLAN_CN_API_KEY"}, true, false, true},
        ProviderDefinition{"qwen-token-plan-individual", "Qwen Token Plan Individual", "https://token-plan.ap-southeast-1.maas.aliyuncs.com/compatible-mode/v1", {"QWEN_TOKEN_PLAN_API_KEY"}, true, false, true},
        ProviderDefinition{"qwen-token-plan", "Qwen Token Plan", "https://token-plan.ap-southeast-1.maas.aliyuncs.com/compatible-mode/v1", {"QWEN_TOKEN_PLAN_API_KEY"}, true, false, true},
        ProviderDefinition{"radius", "Radius", "", {"RADIUS_API_KEY"}, true, true, true},
        ProviderDefinition{"together", "Together", "https://api.together.ai/v1", {"TOGETHER_API_KEY"}, true, false, true},
        ProviderDefinition{"typesafe", "TypeSafe", "", {"TYPESAFE_API_KEY"}, true, false, true},
        ProviderDefinition{"vercel-ai-gateway", "Vercel AI Gateway", "https://ai-gateway.vercel.sh", {"AI_GATEWAY_API_KEY"}, true, false, true},
        ProviderDefinition{"xai", "xAI", "https://api.x.ai/v1", {"XAI_API_KEY"}, true, true, true},
        ProviderDefinition{"xiaomi-token-plan-ams", "Xiaomi Token Plan AMS", "https://token-plan-ams.xiaomimimo.com/v1", {"XIAOMI_TOKEN_PLAN_AMS_API_KEY"}, true, false, true},
        ProviderDefinition{"xiaomi-token-plan-cn", "Xiaomi Token Plan CN", "https://token-plan-cn.xiaomimimo.com/v1", {"XIAOMI_TOKEN_PLAN_CN_API_KEY"}, true, false, true},
        ProviderDefinition{"xiaomi-token-plan-sgp", "Xiaomi Token Plan SGP", "https://token-plan-sgp.xiaomimimo.com/v1", {"XIAOMI_TOKEN_PLAN_SGP_API_KEY"}, true, false, true},
        ProviderDefinition{"xiaomi", "Xiaomi", "https://api.xiaomimimo.com/v1", {"XIAOMI_API_KEY"}, true, false, true},
        ProviderDefinition{"zai-coding-cn", "Z.AI Coding CN", "https://open.bigmodel.cn/api/coding/paas/v4", {"ZAI_CODING_CN_API_KEY"}, true, false, true},
        ProviderDefinition{"zai", "Z.AI", "https://api.z.ai/api/coding/paas/v4", {"ZAI_API_KEY"}, true, false, true},
    };
}

std::optional<ProviderDefinition> BuiltinProviderTable::find(const std::string& id) const {
    for (auto& definition : all()) {
        if (definition.id == id) {
            return definition;
        }
    }
    return std::nullopt;
}
