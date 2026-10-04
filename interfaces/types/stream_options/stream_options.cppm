module;

#include <cstdint>

export module pi.types.stream_options;

import std;
export import pi.support.abort_signal;
export import pi.telemetry.i_telemetry_context;
export import pi.types.json;
export import pi.types.model;
export import pi.types.provider_response;
export import pi.types.thinking_level;

/** Per-request options shared by all providers (the TS StreamOptions + SimpleStreamOptions). */
export struct StreamOptions {
    std::shared_ptr<AbortSignal> signal;
    std::optional<std::string> apiKey;
    /** Provider-scoped environment values; take precedence over the process environment. */
    std::map<std::string, std::string> env;
    /** Caller headers over provider defaults; a nullopt value suppresses a default header. */
    std::vector<std::pair<std::string, std::optional<std::string>>> headers;
    /** Inspect or replace the request payload; return nullopt to keep it. */
    std::function<std::optional<Json>(const Json&, const Model&)> onPayload;
    std::function<void(const ProviderResponse&, const Model&)> onResponse;
    /** Sees every JSON event of a server-sent-events response as parsed, before it is interpreted (not for AWS event streams and WebSockets). */
    std::function<void(const Json&, const Model&)> onStreamEvent;
    /**
     * Runs last over the fully assembled request headers (model, auth and caller headers) and returns the headers to send; the
     * model-runtime applies it, providers never see it.
     */
    std::function<std::vector<std::pair<std::string, std::optional<std::string>>>(const Model&, const std::vector<std::pair<std::string, std::optional<std::string>>>&)> transformHeaders;
    /** Explicit parent context for telemetry produced by this logical request (not used by the providers themselves, like in TypeScript). */
    std::shared_ptr<ITelemetryContext> telemetryContext;
    std::optional<std::int64_t> timeoutMs;
    std::optional<int> maxRetries;
    std::optional<std::int64_t> maxRetryDelayMs;
    std::optional<std::int64_t> websocketConnectTimeoutMs;
    std::optional<double> temperature;
    /** Extra sampling parameters merged into the request body (OpenAI-compatible APIs). */
    Json samplingParams;
    std::optional<std::int64_t> maxTokens;
    /** "sse" | "websocket" | "websocket-cached" | "auto". */
    std::optional<std::string> transport;
    /** "none" | "short" | "long". */
    std::optional<std::string> cacheRetention;
    std::optional<std::string> sessionId;
    Json metadata;
    /** "auto" | "none". */
    std::optional<std::string> toolChoice;
    /** Requested reasoning effort; Off means none. */
    ThinkingLevel reasoning = ThinkingLevel::Off;
    /** Ask a capable provider for a deferred response: true, or {"window": "15m|1h|24h"}. */
    Json deferred;
    /** Token budgets per thinking level: {"minimal":n,"low":n,"medium":n,"high":n}. */
    Json thinkingBudgets;
};
