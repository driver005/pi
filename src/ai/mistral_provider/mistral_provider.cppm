module;

#include <nlohmann/json.hpp>

export module pi.ai.mistral_provider;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_executor;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
export import pi.provider.i_provider;
export import pi.support.assistant_stream_emitter;
export import pi.support.header_merger;
export import pi.support.json_writer;
export import pi.support.mistral_request_builder;
export import pi.support.mistral_stream_reader;
export import pi.support.provider_error_formatter;
export import pi.support.retrying_http_sender;
export import pi.support.sse_request_runner;
export import pi.types.json;

/**
 * Mistral native chat completions (mistral-conversations): POST {baseUrl}/v1/chat/completions
 * with SSE streaming. Port of api/mistral-conversations.ts. Differences: requests go through the
 * shared retrying sender (the TS client does not retry), and the 60 s default `timeoutMs` bounds
 * the silence between received bytes instead of the whole response.
 */
export class MistralProvider : public IProvider {
public:
    MistralProvider(IHttpClient& http, ISleeper& sleeper, const IClock& clock, IExecutor& executor);

    std::string api() const override;
    std::shared_ptr<AssistantMessageStream> stream(const Model& model, const TranscriptContext& context,
                                                   const StreamOptions& options) override;

private:
    void run(const std::shared_ptr<AssistantStreamEmitter>& emitter, const Model& model,
             const TranscriptContext& context, const StreamOptions& options);
    AssistantMessage initialMessage(const Model& model) const;
    std::string chatUrl(const Model& model) const;
    HttpHeaders requestHeaders(const Model& model, const StreamOptions& options, const std::string& apiKey) const;
    bool hasExplicitAffinity(const Model& model, const StreamOptions& options) const;
    std::string formatError(const HttpResponse& response) const;

    IHttpClient& m_http;
    ISleeper& m_sleeper;
    const IClock& m_clock;
    IExecutor& m_executor;
    MistralRequestBuilder m_builder;
    HeaderMerger m_headers;
    JsonWriter m_writer;
    ProviderErrorFormatter m_formatter;
};

MistralProvider::MistralProvider(IHttpClient& http, ISleeper& sleeper, const IClock& clock, IExecutor& executor)
    : m_http(http), m_sleeper(sleeper), m_clock(clock), m_executor(executor) {}

std::string MistralProvider::api() const {
    return "mistral-conversations";
}

AssistantMessage MistralProvider::initialMessage(const Model& model) const {
    AssistantMessage message;
    message.api = model.api;
    message.provider = model.provider;
    message.model = model.id;
    message.stopReason = StopReason::Pending;
    message.timestamp = m_clock.nowMs();
    return message;
}

std::shared_ptr<AssistantMessageStream> MistralProvider::stream(const Model& model, const TranscriptContext& context,
                                                                const StreamOptions& options) {
    auto emitter = std::make_shared<AssistantStreamEmitter>(initialMessage(model));
    auto stream = emitter->stream();
    m_executor.submit([this, emitter, model, context, options]() { run(emitter, model, context, options); });
    return stream;
}

std::string MistralProvider::chatUrl(const Model& model) const {
    std::string base = model.baseUrl;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + "/v1/chat/completions";
}

bool MistralProvider::hasExplicitAffinity(const Model& model, const StreamOptions& options) const {
    return m_headers.find(m_headers.merge({}, model.headers), "x-affinity").has_value() ||
           m_headers.find(m_headers.merge({}, options.headers), "x-affinity").has_value();
}

HttpHeaders MistralProvider::requestHeaders(const Model& model, const StreamOptions& options,
                                            const std::string& apiKey) const {
    HttpHeaders headers = {{"User-Agent", "pi"},
                           {"accept", "text/event-stream"},
                           {"authorization", "Bearer " + apiKey},
                           {"content-type", "application/json"}};
    headers = m_headers.merge(headers, model.headers);
    headers = m_headers.merge(headers, options.headers);
    if (m_builder.usesPromptCaching(options) && !hasExplicitAffinity(model, options)) {
        m_headers.set(headers, "x-affinity", *options.sessionId);
    }
    return headers;
}

std::string MistralProvider::formatError(const HttpResponse& response) const {
    const std::string status = std::to_string(response.status);
    const std::string body = m_formatter.truncate(response.body, ProviderErrorFormatter::MaxBodyChars);
    const auto first = body.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) {
        return "Mistral API error (" + status + "): " + body.substr(first, body.find_last_not_of(" \t\r\n") - first + 1);
    }
    return "Mistral API error (" + status + "): Request failed with status " + status;
}

void MistralProvider::run(const std::shared_ptr<AssistantStreamEmitter>& emitter, const Model& model,
                          const TranscriptContext& context, const StreamOptions& options) {
    if (!options.apiKey || options.apiKey->empty()) {
        emitter->error(StopReason::Error, "No API key for provider: " + model.provider);
        return;
    }
    Json body = m_builder.build(model, context, options, m_clock.nowMs());
    if (options.onPayload) {
        if (auto replaced = options.onPayload(body, model)) {
            body = std::move(*replaced);
        }
    }
    HttpRequest request;
    request.method = "POST";
    request.url = chatUrl(model);
    request.headers = requestHeaders(model, options, *options.apiKey);
    request.body = m_writer.compact(body);
    request.idleTimeout = std::chrono::milliseconds(options.timeoutMs.value_or(60000));
    RetryingHttpSender sender(m_http, m_sleeper, m_clock);
    SseRequestRunner runner(sender);
    MistralStreamReader reader(*emitter, model);
    runner.run(
        std::move(request), model, options, *emitter, [&reader](const SseEvent& event) { return reader.handle(event); },
        [&reader]() { return reader.finish(); }, [this](const HttpResponse& response) { return formatError(response); });
}
