export module pi.support.chat_completions_compat_resolver;

import std;
export import pi.types.model;
export import pi.types.chat_completions_compat;

/**
 * Detects endpoint quirks from provider name and base URL, then applies the model's explicit
 * `compat` overrides. Port of detectCompat/getCompat in api/openai-completions.ts.
 */
export class ChatCompletionsCompatResolver {
public:
    ChatCompletionsCompat resolve(const Model& model) const {
        ChatCompletionsCompat compat = detect(model);
        if (model.compat.is_object()) {
            applyOverrides(compat, model.compat);
        }
        return compat;
    }

    ChatCompletionsCompat detect(const Model& model) const {
        const std::string& provider = model.provider;
        const std::string& baseUrl = model.baseUrl;
        const bool isZai = provider == "zai" || provider == "zai-coding-cn" || contains(baseUrl, "api.z.ai") ||
                           contains(baseUrl, "open.bigmodel.cn");
        const bool isTogether = provider == "together" || contains(baseUrl, "api.together.ai") ||
                                contains(baseUrl, "api.together.xyz");
        const bool isMoonshot = provider == "moonshotai" || provider == "moonshotai-cn" ||
                                contains(baseUrl, "api.moonshot.");
        const bool isOpenRouter = provider == "openrouter" || contains(baseUrl, "openrouter.ai");
        const bool isCloudflareWorkersAi =
            provider == "cloudflare-workers-ai" || contains(baseUrl, "api.cloudflare.com");
        const bool isCloudflareGateway =
            provider == "cloudflare-ai-gateway" || contains(baseUrl, "gateway.ai.cloudflare.com");
        const bool isNvidia = provider == "nvidia" || contains(baseUrl, "integrate.api.nvidia.com");
        const bool isAntLing = provider == "ant-ling" || contains(baseUrl, "api.ant-ling.com");
        const bool isCerebras = provider == "cerebras" || contains(baseUrl, "cerebras.ai");
        const bool isDeepSeek = provider == "deepseek" || contains(lower(baseUrl), "deepseek.com");
        const bool isGrok = provider == "xai" || contains(baseUrl, "api.x.ai");

        const bool isNonStandard = isNvidia || isCerebras || isGrok || isTogether ||
                                   contains(baseUrl, "chutes.ai") || isDeepSeek || isZai || isMoonshot ||
                                   provider == "opencode" || contains(baseUrl, "opencode.ai") ||
                                   isCloudflareWorkersAi || isCloudflareGateway || isAntLing;
        const bool useMaxTokens = contains(baseUrl, "chutes.ai") || isDeepSeek || isMoonshot ||
                                  isCloudflareGateway || isTogether || isNvidia || isAntLing || isZai;
        const bool developerRoleModel =
            isOpenRouter && (startsWith(model.id, "anthropic/") || startsWith(model.id, "openai/"));

        ChatCompletionsCompat compat;
        compat.supportsStore = !isNonStandard;
        compat.supportsDeveloperRole = developerRoleModel || (!isNonStandard && !isOpenRouter);
        compat.supportsReasoningEffort = !isGrok && !isZai && !isMoonshot && !isTogether &&
                                         !isCloudflareGateway && !isNvidia && !isAntLing;
        compat.maxTokensField = useMaxTokens ? "max_tokens" : "max_completion_tokens";
        compat.requiresReasoningContentOnAssistantMessages = isDeepSeek;
        compat.thinkingFormat = isDeepSeek     ? "deepseek"
                                : isZai        ? "zai"
                                : isTogether   ? "together"
                                : isAntLing    ? "ant-ling"
                                : isOpenRouter ? "openrouter"
                                               : "openai";
        if (provider == "openrouter" && startsWith(model.id, "anthropic/")) {
            compat.cacheControlFormat = "anthropic";
        }
        compat.sendSessionAffinityHeaders = isOpenRouter;
        compat.sessionAffinityFormat = isOpenRouter ? "openrouter" : "openai";
        compat.supportsLongCacheRetention = !(isTogether || isCloudflareWorkersAi || isCloudflareGateway ||
                                              isNvidia || isAntLing);
        return compat;
    }

private:
    bool contains(const std::string& text, const std::string& part) const {
        return text.find(part) != std::string::npos;
    }

    bool startsWith(const std::string& text, const std::string& prefix) const {
        return text.rfind(prefix, 0) == 0;
    }

    std::string lower(const std::string& text) const {
        std::string out = text;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    void applyOverrides(ChatCompletionsCompat& compat, const Json& overrides) const {
        overrideBool(compat.supportsStore, overrides, "supportsStore");
        overrideBool(compat.supportsDeveloperRole, overrides, "supportsDeveloperRole");
        overrideBool(compat.supportsReasoningEffort, overrides, "supportsReasoningEffort");
        overrideBool(compat.supportsUsageInStreaming, overrides, "supportsUsageInStreaming");
        overrideBool(compat.supportsFinishReason, overrides, "supportsFinishReason");
        overrideString(compat.maxTokensField, overrides, "maxTokensField");
        overrideBool(compat.requiresToolResultName, overrides, "requiresToolResultName");
        overrideBool(compat.requiresAssistantAfterToolResult, overrides, "requiresAssistantAfterToolResult");
        overrideBool(compat.requiresThinkingAsText, overrides, "requiresThinkingAsText");
        overrideBool(compat.requiresReasoningContentOnAssistantMessages, overrides,
                     "requiresReasoningContentOnAssistantMessages");
        overrideString(compat.thinkingFormat, overrides, "thinkingFormat");
        overrideJson(compat.openRouterRouting, overrides, "openRouterRouting");
        overrideJson(compat.vercelGatewayRouting, overrides, "vercelGatewayRouting");
        overrideJson(compat.chatTemplateKwargs, overrides, "chatTemplateKwargs");
        overrideJson(compat.chatTemplateArgs, overrides, "chatTemplateArgs");
        overrideBool(compat.zaiToolStream, overrides, "zaiToolStream");
        overrideBool(compat.supportsThinkingTokenBudget, overrides, "supportsThinkingTokenBudget");
        if (overrides.contains("thinkingTokenBudgetField") && overrides["thinkingTokenBudgetField"].is_string()) {
            compat.thinkingTokenBudgetField = overrides["thinkingTokenBudgetField"].get<std::string>();
        }
        overrideBool(compat.supportsStrictMode, overrides, "supportsStrictMode");
        if (overrides.contains("cacheControlFormat") && overrides["cacheControlFormat"].is_string()) {
            compat.cacheControlFormat = overrides["cacheControlFormat"].get<std::string>();
        }
        overrideBool(compat.sendSessionAffinityHeaders, overrides, "sendSessionAffinityHeaders");
        overrideString(compat.sessionAffinityFormat, overrides, "sessionAffinityFormat");
        overrideBool(compat.supportsLongCacheRetention, overrides, "supportsLongCacheRetention");
        overrideJson(compat.vllmPriority, overrides, "vllmPriority");
    }

    void overrideBool(bool& target, const Json& overrides, const std::string& key) const {
        if (overrides.contains(key) && overrides[key].is_boolean()) {
            target = overrides[key].get<bool>();
        }
    }

    void overrideString(std::string& target, const Json& overrides, const std::string& key) const {
        if (overrides.contains(key) && overrides[key].is_string()) {
            target = overrides[key].get<std::string>();
        }
    }

    void overrideJson(Json& target, const Json& overrides, const std::string& key) const {
        if (overrides.contains(key) && !overrides[key].is_null()) {
            target = overrides[key];
        }
    }
};
