export module pi.ai.google_provider;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_executor;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
export import pi.provider.i_provider;
export import pi.support.assistant_stream_emitter;
export import pi.support.google_request_builder;
export import pi.support.google_stream_reader;
export import pi.support.header_merger;
export import pi.support.json_writer;
export import pi.support.provider_error_formatter;
export import pi.support.retrying_http_sender;
export import pi.support.sse_request_runner;
export import pi.types.json;

/**
 * Google Generative AI (google-generative-ai): POST {baseUrl}/models/{id}:streamGenerateContent
 * with `alt=sse` and the `x-goog-api-key` header. Port of api/google-generative-ai.ts.
 */
export class GoogleProvider : public IProvider {
public:
    GoogleProvider(IHttpClient& http, ISleeper& sleeper, const IClock& clock, IExecutor& executor);

    std::string api() const override;
    std::shared_ptr<AssistantMessageStream> stream(const Model& model, const TranscriptContext& context,
                                                   const StreamOptions& options) override;

private:
    void run(const std::shared_ptr<AssistantStreamEmitter>& emitter, const Model& model,
             const TranscriptContext& context, const StreamOptions& options);
    AssistantMessage initialMessage(const Model& model) const;
    std::string streamUrl(const Model& model) const;
    HttpRequest buildRequest(const Model& model, Json body, const StreamOptions& options,
                             const std::string& apiKey) const;
    std::string formatError(const HttpResponse& response) const;

    IHttpClient& m_http;
    ISleeper& m_sleeper;
    const IClock& m_clock;
    IExecutor& m_executor;
    GoogleRequestBuilder m_builder;
    HeaderMerger m_headers;
    JsonWriter m_writer;
    ProviderErrorFormatter m_formatter;
};

GoogleProvider::GoogleProvider(IHttpClient& http, ISleeper& sleeper, const IClock& clock, IExecutor& executor)
    : m_http(http), m_sleeper(sleeper), m_clock(clock), m_executor(executor) {}

std::string GoogleProvider::api() const {
    return "google-generative-ai";
}

AssistantMessage GoogleProvider::initialMessage(const Model& model) const {
    AssistantMessage message;
    message.api = "google-generative-ai";
    message.provider = model.provider;
    message.model = model.id;
    message.stopReason = StopReason::Pending;
    message.timestamp = m_clock.nowMs();
    return message;
}

std::shared_ptr<AssistantMessageStream> GoogleProvider::stream(const Model& model, const TranscriptContext& context,
                                                               const StreamOptions& options) {
    auto emitter = std::make_shared<AssistantStreamEmitter>(initialMessage(model));
    auto stream = emitter->stream();
    m_executor.submit([this, emitter, model, context, options]() { run(emitter, model, context, options); });
    return stream;
}

std::string GoogleProvider::streamUrl(const Model& model) const {
    std::string base = model.baseUrl.empty() ? "https://generativelanguage.googleapis.com/v1beta" : model.baseUrl;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    const bool qualified = model.id.find('/') != std::string::npos;
    return base + "/" + (qualified ? model.id : "models/" + model.id) + ":streamGenerateContent?alt=sse";
}

HttpRequest GoogleProvider::buildRequest(const Model& model, Json body, const StreamOptions& options,
                                         const std::string& apiKey) const {
    if (options.onPayload) {
        if (auto replaced = options.onPayload(body, model)) {
            body = std::move(*replaced);
        }
    }
    HttpRequest request;
    request.method = "POST";
    request.url = streamUrl(model);
    HttpHeaders headers = {{"User-Agent", "pi"}};
    headers = m_headers.merge(headers, model.headers);
    headers = m_headers.merge(headers, options.headers);
    m_headers.set(headers, "content-type", "application/json");
    m_headers.set(headers, "accept", "text/event-stream");
    m_headers.set(headers, "x-goog-api-key", apiKey);
    request.headers = headers;
    request.body = m_writer.compact(body);
    if (options.timeoutMs) {
        request.idleTimeout = std::chrono::milliseconds(*options.timeoutMs);
    }
    return request;
}

std::string GoogleProvider::formatError(const HttpResponse& response) const {
    // The Google SDK reports the error body itself as the message.
    const Json body = Json::parse(response.body, nullptr, false);
    if (!body.is_discarded()) {
        return m_formatter.truncate(body.dump(-1, ' ', false, Json::error_handler_t::replace),
                                    ProviderErrorFormatter::MaxBodyChars);
    }
    return m_formatter.formatHttp(response);
}

void GoogleProvider::run(const std::shared_ptr<AssistantStreamEmitter>& emitter, const Model& model,
                         const TranscriptContext& context, const StreamOptions& options) {
    if (!options.apiKey || options.apiKey->empty()) {
        emitter->error(StopReason::Error, "No API key for provider: " + model.provider);
        return;
    }
    auto body = m_builder.build(model, context, options, m_clock.nowMs());
    if (!body) {
        emitter->error(StopReason::Error, body.error().message);
        return;
    }
    RetryingHttpSender sender(m_http, m_sleeper, m_clock);
    SseRequestRunner runner(sender);
    GoogleStreamReader reader(*emitter, model, m_clock);
    runner.run(
        buildRequest(model, std::move(*body), options, *options.apiKey), model, options, *emitter,
        [&reader](const SseEvent& event) { return reader.handle(event); }, [&reader]() { return reader.finish(); },
        [this](const HttpResponse& response) { return formatError(response); });
}
