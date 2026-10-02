module;

#include <nlohmann/json.hpp>

export module pi.types.chat_completions_compat;

import std;
export import pi.types.json;

/** Resolved quirks of an OpenAI-compatible chat completions endpoint. */
export struct ChatCompletionsCompat {
    bool supportsStore = true;
    bool supportsDeveloperRole = true;
    bool supportsReasoningEffort = true;
    bool supportsUsageInStreaming = true;
    bool supportsFinishReason = true;
    /** "max_tokens" | "max_completion_tokens". */
    std::string maxTokensField = "max_completion_tokens";
    bool requiresToolResultName = false;
    bool requiresAssistantAfterToolResult = false;
    bool requiresThinkingAsText = false;
    bool requiresReasoningContentOnAssistantMessages = false;
    /** openai deepseek zai together ant-ling openrouter qwen qwen-chat-template chat-template ... */
    std::string thinkingFormat = "openai";
    Json openRouterRouting = Json::object();
    Json vercelGatewayRouting = Json::object();
    Json chatTemplateKwargs = Json::object();
    Json chatTemplateArgs = Json::object();
    bool zaiToolStream = false;
    bool supportsThinkingTokenBudget = false;
    std::optional<std::string> thinkingTokenBudgetField;
    bool supportsStrictMode = false;
    std::optional<std::string> cacheControlFormat;
    bool sendSessionAffinityHeaders = false;
    /** "openai" | "openrouter". */
    std::string sessionAffinityFormat = "openai";
    bool supportsLongCacheRetention = true;
    Json vllmPriority;
};
