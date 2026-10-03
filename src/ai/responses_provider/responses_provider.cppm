export module pi.ai.responses_provider;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_executor;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
export import pi.provider.i_provider;
export import pi.support.assistant_stream_emitter;
export import pi.support.copilot_headers;
export import pi.support.header_merger;
export import pi.support.json_writer;
export import pi.support.provider_error_formatter;
export import pi.support.responses_compat_resolver;
export import pi.support.responses_request_builder;
export import pi.support.responses_stream_reader;
export import pi.support.retrying_http_sender;
export import pi.support.sse_request_runner;
export import pi.types.json;

/**
 * OpenAI Responses (openai-responses): POST {baseUrl}/responses with SSE streaming. Port of
 * packages/ai/src/api/openai-responses.ts.
 */
export class ResponsesProvider : public IProvider {
public:
    ResponsesProvider(IHttpClient& http, ISleeper& sleeper, const IClock& clock, IExecutor& executor);

    std::string api() const override;
    std::shared_ptr<AssistantMessageStream> stream(const Model& model, const TranscriptContext& context,
                                                   const StreamOptions& options) override;

private:
    void run(const std::shared_ptr<AssistantStreamEmitter>& emitter, const Model& model,
             const TranscriptContext& context, const StreamOptions& options);
    AssistantMessage initialMessage(const Model& model) const;
    bool hasHeaderAuth(const Model& model, const StreamOptions& options) const;
    HttpHeaders requestHeaders(const Model& model, const TranscriptContext& context, const StreamOptions& options,
                               const std::string& apiKey) const;
    HttpRequest buildRequest(const Model& model, const TranscriptContext& context, const StreamOptions& options,
                             const std::string& apiKey) const;
    std::string responsesUrl(const Model& model) const;
    std::string formatError(const HttpResponse& response) const;

    IHttpClient& m_http;
    ISleeper& m_sleeper;
    const IClock& m_clock;
    IExecutor& m_executor;
    ResponsesRequestBuilder m_builder;
    ResponsesCompatResolver m_compat;
    CopilotHeaders m_copilot;
    HeaderMerger m_headers;
    JsonWriter m_writer;
    ProviderErrorFormatter m_formatter;
};

ResponsesProvider::ResponsesProvider(IHttpClient& http, ISleeper& sleeper, const IClock& clock, IExecutor& executor)
    : m_http(http), m_sleeper(sleeper), m_clock(clock), m_executor(executor) {}

std::string ResponsesProvider::api() const {
    return "openai-responses";
}

AssistantMessage ResponsesProvider::initialMessage(const Model& model) const {
    AssistantMessage message;
    message.api = model.api;
    message.provider = model.provider;
    message.model = model.id;
    message.stopReason = StopReason::Pending;
    message.timestamp = m_clock.nowMs();
    return message;
}

std::shared_ptr<AssistantMessageStream> ResponsesProvider::stream(const Model& model,
                                                                  const TranscriptContext& context,
                                                                  const StreamOptions& options) {
    auto emitter = std::make_shared<AssistantStreamEmitter>(initialMessage(model));
    auto stream = emitter->stream();
    m_executor.submit([this, emitter, model, context, options]() { run(emitter, model, context, options); });
    return stream;
}

bool ResponsesProvider::hasHeaderAuth(const Model& model, const StreamOptions& options) const {
    const HttpHeaders merged = m_headers.merge(m_headers.merge({}, model.headers), options.headers);
    for (const std::string name : {"authorization", "cf-aig-authorization"}) {
        const auto value = m_headers.find(merged, name);
        if (value && value->find_first_not_of(" \t") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string ResponsesProvider::responsesUrl(const Model& model) const {
    std::string base = model.baseUrl;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + "/responses";
}

HttpHeaders ResponsesProvider::requestHeaders(const Model& model, const TranscriptContext& context,
                                              const StreamOptions& options, const std::string& apiKey) const {
    const ResponsesCompat compat = m_compat.resolve(model);
    HttpHeaders headers = {{"User-Agent", "pi"}};
    headers = m_headers.merge(headers, model.headers);
    if (model.provider == "github-copilot") {
        for (const auto& entry : m_copilot.dynamicHeaders(context.messages)) {
            m_headers.set(headers, entry.first, entry.second);
        }
    }
    if (options.sessionId && m_builder.retention(options) != "none") {
        if (compat.sessionAffinityFormat == "openrouter") {
            m_headers.set(headers, "x-session-id", *options.sessionId);
        } else {
            if (compat.sessionAffinityFormat == "openai") {
                m_headers.set(headers, "session_id", *options.sessionId);
            }
            m_headers.set(headers, "x-client-request-id", *options.sessionId);
        }
    }
    headers = m_headers.merge(headers, options.headers);
    m_headers.set(headers, "content-type", "application/json");
    m_headers.set(headers, "accept", "application/json");
    if (!m_headers.find(headers, "authorization") || apiKey != "unused") {
        m_headers.set(headers, "authorization", "Bearer " + apiKey);
    }
    return headers;
}

HttpRequest ResponsesProvider::buildRequest(const Model& model, const TranscriptContext& context,
                                            const StreamOptions& options, const std::string& apiKey) const {
    StreamOptions effective = options;
    effective.apiKey = apiKey == "unused" ? std::nullopt : std::optional<std::string>(apiKey);
    Json body = m_builder.build(model, context, effective, m_clock.nowMs());
    if (options.onPayload) {
        if (auto replaced = options.onPayload(body, model)) {
            body = std::move(*replaced);
        }
    }
    HttpRequest request;
    request.method = "POST";
    request.url = responsesUrl(model);
    request.headers = requestHeaders(model, context, options, apiKey);
    request.body = m_writer.compact(body);
    if (options.timeoutMs) {
        request.idleTimeout = std::chrono::milliseconds(*options.timeoutMs);
    }
    return request;
}

std::string ResponsesProvider::formatError(const HttpResponse& response) const {
    const std::string message = m_formatter.formatOpenAi(response);
    // Sign in with ChatGPT shares the subscription's usage limit with other apps.
    const std::string marker = "subscription_sharing_usage_limit_exceeded";
    if (message.find(marker) != std::string::npos || response.body.find(marker) != std::string::npos) {
        return message + "\nCheck your ChatGPT usage: https://chatgpt.com/settings/usage";
    }
    return message;
}

void ResponsesProvider::run(const std::shared_ptr<AssistantStreamEmitter>& emitter, const Model& model,
                            const TranscriptContext& context, const StreamOptions& options) {
    std::string apiKey;
    if (options.apiKey && !options.apiKey->empty()) {
        apiKey = *options.apiKey;
    } else if (hasHeaderAuth(model, options)) {
        apiKey = "unused";
    } else {
        emitter->error(StopReason::Error, "No API key for provider: " + model.provider);
        return;
    }
    RetryingHttpSender sender(m_http, m_sleeper, m_clock);
    SseRequestRunner runner(sender);
    ResponsesStreamReader reader(*emitter, model);
    runner.run(
        buildRequest(model, context, options, apiKey), model, options, *emitter,
        [&reader](const SseEvent& event) { return reader.handle(event); }, [&reader]() { return reader.finish(); },
        [this](const HttpResponse& response) { return formatError(response); });
}
