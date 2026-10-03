export module pi.ai.google_vertex_provider;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_environment;
export import pi.platform.i_executor;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
export import pi.provider.i_access_token_source;
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
 * Google Vertex AI (google-vertex): streamGenerateContent on aiplatform.googleapis.com. With a
 * Vertex API key the express endpoint is used; otherwise the project and location come from
 * GOOGLE_CLOUD_PROJECT / GCLOUD_PROJECT and GOOGLE_CLOUD_LOCATION and the bearer token from the
 * injected token source (Application Default Credentials). Port of api/google-vertex.ts.
 */
export class GoogleVertexProvider : public IProvider {
public:
    GoogleVertexProvider(IHttpClient& http, ISleeper& sleeper, const IClock& clock, IExecutor& executor,
                         const IEnvironment& environment, IAccessTokenSource& tokens);

    std::string api() const override;
    std::shared_ptr<AssistantMessageStream> stream(const Model& model, const TranscriptContext& context,
                                                   const StreamOptions& options) override;

private:
    void run(const std::shared_ptr<AssistantStreamEmitter>& emitter, const Model& model,
             const TranscriptContext& context, const StreamOptions& options);
    AssistantMessage initialMessage(const Model& model) const;
    std::optional<std::string> envValue(const StreamOptions& options, const std::string& name) const;
    std::optional<std::string> usableApiKey(const StreamOptions& options) const;
    std::string trimmed(const std::string& text) const;
    std::string modelPath(const std::string& id) const;
    bool pathHasVersion(const std::string& path) const;
    Result<std::string> streamUrl(const Model& model, const StreamOptions& options, bool apiKeyMode) const;
    Result<std::string> customUrl(const std::string& base, const std::string& model) const;
    HttpRequest buildRequest(const Model& model, Json body, const StreamOptions& options, const std::string& url,
                             const std::string& credential, bool apiKeyMode) const;
    std::string formatError(const HttpResponse& response) const;

    IHttpClient& m_http;
    ISleeper& m_sleeper;
    const IClock& m_clock;
    IExecutor& m_executor;
    const IEnvironment& m_environment;
    IAccessTokenSource& m_tokens;
    GoogleRequestBuilder m_builder;
    HeaderMerger m_headers;
    JsonWriter m_writer;
    ProviderErrorFormatter m_formatter;
};

GoogleVertexProvider::GoogleVertexProvider(IHttpClient& http, ISleeper& sleeper, const IClock& clock,
                                           IExecutor& executor, const IEnvironment& environment,
                                           IAccessTokenSource& tokens)
    : m_http(http),
      m_sleeper(sleeper),
      m_clock(clock),
      m_executor(executor),
      m_environment(environment),
      m_tokens(tokens) {}

std::string GoogleVertexProvider::api() const {
    return "google-vertex";
}

AssistantMessage GoogleVertexProvider::initialMessage(const Model& model) const {
    AssistantMessage message;
    message.api = "google-vertex";
    message.provider = model.provider;
    message.model = model.id;
    message.stopReason = StopReason::Pending;
    message.timestamp = m_clock.nowMs();
    return message;
}

std::shared_ptr<AssistantMessageStream> GoogleVertexProvider::stream(const Model& model,
                                                                     const TranscriptContext& context,
                                                                     const StreamOptions& options) {
    auto emitter = std::make_shared<AssistantStreamEmitter>(initialMessage(model));
    auto stream = emitter->stream();
    m_executor.submit([this, emitter, model, context, options]() { run(emitter, model, context, options); });
    return stream;
}

std::string GoogleVertexProvider::trimmed(const std::string& text) const {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::optional<std::string> GoogleVertexProvider::envValue(const StreamOptions& options, const std::string& name) const {
    const auto scoped = options.env.find(name);
    if (scoped != options.env.end() && !scoped->second.empty()) {
        return scoped->second;
    }
    const auto process = m_environment.get(name);
    return process && !process->empty() ? process : std::nullopt;
}

std::optional<std::string> GoogleVertexProvider::usableApiKey(const StreamOptions& options) const {
    if (!options.apiKey) {
        return std::nullopt;
    }
    const std::string key = trimmed(*options.apiKey);
    const bool placeholder = key.size() > 2 && key.front() == '<' && key.back() == '>' &&
                             key.find('>') == key.size() - 1;
    if (key.empty() || key == "gcp-vertex-credentials" || placeholder) {
        return std::nullopt;
    }
    return key;
}

std::string GoogleVertexProvider::modelPath(const std::string& id) const {
    if (id.starts_with("publishers/") || id.starts_with("projects/")) {
        return id;
    }
    const auto slash = id.find('/');
    if (slash == std::string::npos) {
        return "publishers/google/models/" + id;
    }
    return "publishers/" + id.substr(0, slash) + "/models/" + id.substr(slash + 1);
}

bool GoogleVertexProvider::pathHasVersion(const std::string& path) const {
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        end = end == std::string::npos ? path.size() : end;
        const std::string part = path.substr(start, end - start);
        if (part.size() >= 2 && part[0] == 'v' && std::isdigit(static_cast<unsigned char>(part[1])) != 0) {
            std::size_t i = 1;
            while (i < part.size() && std::isdigit(static_cast<unsigned char>(part[i])) != 0) {
                ++i;
            }
            if (i == part.size() || part.compare(i, 4, "beta") == 0) {
                return true;
            }
        }
        start = end + 1;
    }
    return false;
}

Result<std::string> GoogleVertexProvider::customUrl(const std::string& rawBase, const std::string& model) const {
    std::string base = trimmed(rawBase);
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    const auto scheme = base.find("://");
    const auto pathStart = scheme == std::string::npos ? std::string::npos : base.find('/', scheme + 3);
    const std::string path = pathStart == std::string::npos ? "" : base.substr(pathStart);
    const std::string version = pathHasVersion(path) ? "" : "/v1";
    return base + version + "/" + modelPath(model) + ":streamGenerateContent?alt=sse";
}

Result<std::string> GoogleVertexProvider::streamUrl(const Model& model, const StreamOptions& options,
                                                    bool apiKeyMode) const {
    const std::string custom = trimmed(model.baseUrl);
    if (!custom.empty() && custom.find("{location}") == std::string::npos) {
        return customUrl(custom, model.id);
    }
    if (apiKeyMode) {
        return "https://aiplatform.googleapis.com/v1/" + modelPath(model.id) + ":streamGenerateContent?alt=sse";
    }
    const auto project = envValue(options, "GOOGLE_CLOUD_PROJECT");
    const auto fallbackProject = project ? project : envValue(options, "GCLOUD_PROJECT");
    if (!fallbackProject) {
        return std::unexpected(Error{"config", "Vertex AI requires a project ID. Set GOOGLE_CLOUD_PROJECT/GCLOUD_PROJECT "
                                               "or pass project in options."});
    }
    const auto location = envValue(options, "GOOGLE_CLOUD_LOCATION");
    if (!location) {
        return std::unexpected(Error{"config", "Vertex AI requires a location. Set GOOGLE_CLOUD_LOCATION or pass "
                                               "location in options."});
    }
    const std::string host = *location == "global" ? "aiplatform.googleapis.com"
                                                   : *location + "-aiplatform.googleapis.com";
    const std::string path = modelPath(model.id);
    const std::string resource = path.starts_with("projects/")
                                     ? path
                                     : "projects/" + *fallbackProject + "/locations/" + *location + "/" + path;
    return "https://" + host + "/v1/" + resource + ":streamGenerateContent?alt=sse";
}

HttpRequest GoogleVertexProvider::buildRequest(const Model& model, Json body, const StreamOptions& options,
                                               const std::string& url, const std::string& credential,
                                               bool apiKeyMode) const {
    if (options.onPayload) {
        if (auto replaced = options.onPayload(body, model)) {
            body = std::move(*replaced);
        }
    }
    HttpRequest request;
    request.method = "POST";
    request.url = url;
    HttpHeaders headers = {{"User-Agent", "pi"}};
    headers = m_headers.merge(headers, model.headers);
    headers = m_headers.merge(headers, options.headers);
    m_headers.set(headers, "content-type", "application/json");
    m_headers.set(headers, "accept", "text/event-stream");
    if (apiKeyMode) {
        m_headers.set(headers, "x-goog-api-key", credential);
    } else {
        m_headers.set(headers, "authorization", "Bearer " + credential);
    }
    request.headers = headers;
    request.body = m_writer.compact(body);
    if (options.timeoutMs) {
        request.idleTimeout = std::chrono::milliseconds(*options.timeoutMs);
    }
    return request;
}

std::string GoogleVertexProvider::formatError(const HttpResponse& response) const {
    const Json body = Json::parse(response.body, nullptr, false);
    if (!body.is_discarded()) {
        return m_formatter.truncate(body.dump(-1, ' ', false, Json::error_handler_t::replace),
                                    ProviderErrorFormatter::MaxBodyChars);
    }
    return m_formatter.formatHttp(response);
}

void GoogleVertexProvider::run(const std::shared_ptr<AssistantStreamEmitter>& emitter, const Model& model,
                               const TranscriptContext& context, const StreamOptions& options) {
    const auto apiKey = usableApiKey(options);
    const auto url = streamUrl(model, options, apiKey.has_value());
    if (!url) {
        emitter->error(StopReason::Error, url.error().message);
        return;
    }
    std::string credential;
    if (apiKey) {
        credential = *apiKey;
    } else {
        const auto token = m_tokens.token(options.env);
        if (!token) {
            emitter->error(StopReason::Error, token.error().message);
            return;
        }
        credential = *token;
    }
    auto body = m_builder.build(model, context, options, m_clock.nowMs(), false);
    if (!body) {
        emitter->error(StopReason::Error, body.error().message);
        return;
    }
    RetryingHttpSender sender(m_http, m_sleeper, m_clock);
    SseRequestRunner runner(sender);
    GoogleStreamReader reader(*emitter, model, m_clock);
    runner.run(
        buildRequest(model, std::move(*body), options, *url, credential, apiKey.has_value()), model, options, *emitter,
        [&reader](const SseEvent& event) { return reader.handle(event); }, [&reader]() { return reader.finish(); },
        [this](const HttpResponse& response) { return formatError(response); });
}
