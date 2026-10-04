export module pi.types.summarization_options;

import std;
export import pi.types.agent_loop_config;
export import pi.types.assistant_retry_policy;
export import pi.types.model;
export import pi.types.retry_callbacks;
export import pi.types.stream_options;
export import pi.types.thinking_level;

/**
 * How a summarization request reaches the model. `stream` carries credentials, headers, env,
 * signal and session id; the generator sets maxTokens, reasoning and cache retention itself.
 * `streamFn` is the session's stream function so SDK request behavior stays consistent.
 */
export struct SummarizationOptions {
    Model model;
    StreamOptions stream;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
    StreamFn streamFn;
    AssistantRetryPolicy retry;
    RetryCallbacks callbacks;
};
