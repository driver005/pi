module;

#include <nlohmann/json.hpp>

export module pi.ai.anthropic_messages_provider;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_executor;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
export import pi.provider.i_provider;
export import pi.support.anthropic_request_builder;
export import pi.support.anthropic_stream_reader;
export import pi.support.assistant_stream_emitter;
export import pi.support.header_merger;
export import pi.support.json_writer;
export import pi.support.provider_error_formatter;
export import pi.support.retrying_http_sender;
export import pi.support.sse_request_runner;
export import pi.support.transcript_normalizer;
export import pi.types.json;

/**
 * Anthropic Messages API (anthropic-messages): POST {baseUrl}/v1/messages with SSE streaming.
 * Port of packages/ai/src/api/anthropic-messages.ts. Auth is an API key (x-api-key) or an OAuth
 * token (Bearer, Claude Code identity); header-owned auth is accepted. Not ported: Anthropic
 * workload identity federation and GitHub Copilot dynamic headers.
 */
export class AnthropicMessagesProvider : public IProvider {
public:
    AnthropicMessagesProvider(IHttpClient& http, ISleeper& sleeper, const IClock& clock,
                              IExecutor& executor);

    std::string api() const override;
    std::shared_ptr<AssistantMessageStream> stream(const Model& model,
                                                   const TranscriptContext& context,
                                                   const StreamOptions& options) override;

private:
    void run(const std::shared_ptr<AssistantStreamEmitter>& emitter, const Model& model,
             const TranscriptContext& context, const StreamOptions& options);
    AssistantMessage initialMessage(const Model& model) const;
    bool hasRequestAuth(const std::optional<std::string>& apiKey, const StreamOptions& options,
                        const Model& model) const;
    HttpHeaders defaultHeaders(const Model& model, const StreamOptions& options, bool isOAuth) const;
    HttpHeaders requestHeaders(const Model& model, const TranscriptContext& context,
                               const StreamOptions& options, bool isOAuth) const;
    HttpRequest buildRequest(const Model& model, const TranscriptContext& context,
                             const StreamOptions& options, bool isOAuth) const;
    std::string messagesUrl(const Model& model) const;

    IHttpClient& m_http;
    ISleeper& m_sleeper;
    const IClock& m_clock;
    IExecutor& m_executor;
    AnthropicRequestBuilder m_builder;
    TranscriptNormalizer m_normalizer;
    HeaderMerger m_headers;
    JsonWriter m_writer;
    ProviderErrorFormatter m_formatter;
};

AnthropicMessagesProvider::AnthropicMessagesProvider(IHttpClient& http, ISleeper& sleeper,
                                                     const IClock& clock, IExecutor& executor)
    : m_http(http), m_sleeper(sleeper), m_clock(clock), m_executor(executor) {}

std::string AnthropicMessagesProvider::api() const {
    return "anthropic-messages";
}

AssistantMessage AnthropicMessagesProvider::initialMessage(const Model& model) const {
    AssistantMessage message;
    message.api = model.api;
    message.provider = model.provider;
    message.model = model.id;
    message.stopReason = StopReason::Pending;
    message.timestamp = m_clock.nowMs();
    return message;
}

std::shared_ptr<AssistantMessageStream> AnthropicMessagesProvider::stream(
    const Model& model, const TranscriptContext& context, const StreamOptions& options) {
    auto emitter = std::make_shared<AssistantStreamEmitter>(initialMessage(model));
    auto stream = emitter->stream();
    m_executor.submit([this, emitter, model, context, options]() {
        run(emitter, model, context, options);
    });
    return stream;
}

bool AnthropicMessagesProvider::hasRequestAuth(const std::optional<std::string>& apiKey,
                                               const StreamOptions& options,
                                               const Model& model) const {
    if (apiKey && !apiKey->empty()) {
        return true;
    }
    const HttpHeaders merged = m_headers.merge(m_headers.merge({}, model.headers), options.headers);
    for (const std::string name : {"authorization", "x-api-key", "cf-aig-authorization"}) {
        const auto value = m_headers.find(merged, name);
        if (value && value->find_first_not_of(" \t") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string AnthropicMessagesProvider::messagesUrl(const Model& model) const {
    std::string base = model.baseUrl;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + "/v1/messages?beta=true";
}

HttpHeaders AnthropicMessagesProvider::defaultHeaders(const Model& model,
                                                      const StreamOptions& options,
                                                      bool isOAuth) const {
    HttpHeaders headers = {{"User-Agent", "pi"},
                           {"accept", "application/json"},
                           {"anthropic-dangerous-direct-browser-access", "true"}};
    if (isOAuth) {
        m_headers.set(headers, "user-agent", "claude-cli/2.1.280");
        m_headers.set(headers, "x-app", "cli");
    }
    const bool copilot = model.provider == "github-copilot";
    const bool openRouter =
        model.provider == "openrouter" || model.baseUrl.find("openrouter.ai") != std::string::npos;
    const bool affinity = model.compat.is_object() && model.compat.contains("sendSessionAffinityHeaders")
                              ? model.compat["sendSessionAffinityHeaders"].get<bool>()
                              : openRouter;
    const bool cacheEnabled = options.cacheRetention.value_or("short") != "none";
    if (!isOAuth && !copilot && affinity && options.sessionId && cacheEnabled) {
        const std::string format =
            model.compat.is_object() && model.compat.contains("sessionAffinityFormat")
                ? model.compat["sessionAffinityFormat"].get<std::string>()
                : (openRouter ? "openrouter" : "");
        headers.emplace_back(format == "openrouter" ? "x-session-id" : "x-session-affinity",
                             *options.sessionId);
    }
    return m_headers.merge(m_headers.merge(headers, model.headers), options.headers);
}

HttpHeaders AnthropicMessagesProvider::requestHeaders(const Model& model,
                                                      const TranscriptContext& context,
                                                      const StreamOptions& options,
                                                      bool isOAuth) const {
    HttpHeaders headers = defaultHeaders(model, options, isOAuth);
    m_headers.set(headers, "content-type", "application/json");
    m_headers.set(headers, "anthropic-version", "2023-06-01");
    if (options.apiKey && !options.apiKey->empty()) {
        if (isOAuth || model.provider == "github-copilot") {
            m_headers.set(headers, "authorization", "Bearer " + *options.apiKey);
        } else {
            m_headers.set(headers, "x-api-key", *options.apiKey);
        }
    }
    const auto betas = m_builder.betaFeatures(model, context, isOAuth, options);
    if (!betas.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < betas.size(); ++i) {
            joined += (i > 0 ? "," : "") + betas[i];
        }
        m_headers.set(headers, "anthropic-beta", joined);
    }
    return headers;
}

HttpRequest AnthropicMessagesProvider::buildRequest(const Model& model,
                                                    const TranscriptContext& context,
                                                    const StreamOptions& options,
                                                    bool isOAuth) const {
    Json body = m_builder.build(model, context, isOAuth, options, m_clock.nowMs());
    if (options.onPayload) {
        if (auto replaced = options.onPayload(body, model)) {
            body = std::move(*replaced);
            body["stream"] = true;
        }
    }
    HttpRequest request;
    request.method = "POST";
    request.url = messagesUrl(model);
    request.headers = requestHeaders(model, context, options, isOAuth);
    request.body = m_writer.compact(body);
    if (options.timeoutMs) {
        request.idleTimeout = std::chrono::milliseconds(*options.timeoutMs);
    }
    return request;
}

void AnthropicMessagesProvider::run(const std::shared_ptr<AssistantStreamEmitter>& emitter,
                                    const Model& model, const TranscriptContext& rawContext,
                                    const StreamOptions& options) {
    if (!hasRequestAuth(options.apiKey, options, model)) {
        emitter->error(StopReason::Error, "No API key for provider: " + model.provider);
        return;
    }
    const bool isOAuth = options.apiKey && m_builder.isOAuthToken(*options.apiKey);
    RetryingHttpSender sender(m_http, m_sleeper, m_clock);
    SseRequestRunner runner(sender);
    AnthropicStreamReader reader(*emitter, model, isOAuth,
                                 m_normalizer.currentTools(rawContext.messages));
    runner.run(
        buildRequest(model, rawContext, options, isOAuth), model, options, *emitter,
        [&reader](const SseEvent& event) { return reader.handle(event); },
        [&reader]() { return reader.finish(); },
        [this](const HttpResponse& response) { return m_formatter.formatSdk(response); });
}
