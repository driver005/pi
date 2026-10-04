export module pi.ai.azure_responses_provider;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_environment;
export import pi.platform.i_executor;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
export import pi.provider.i_provider;
export import pi.support.assistant_stream_emitter;
export import pi.support.header_merger;
export import pi.support.json_writer;
export import pi.support.provider_error_formatter;
export import pi.support.responses_request_builder;
export import pi.support.responses_stream_reader;
export import pi.support.retrying_http_sender;
export import pi.support.sse_request_runner;
export import pi.types.json;

/**
 * Azure OpenAI Responses (azure-openai-responses): POST {base}/responses?api-version=... with the
 * `api-key` header and the deployment name as the model. Port of api/azure-openai-responses.ts.
 *
 * Configuration comes from the provider environment (StreamOptions::env, then the process):
 * AZURE_OPENAI_BASE_URL, AZURE_OPENAI_RESOURCE_NAME, AZURE_OPENAI_API_VERSION (default "v1") and
 * AZURE_OPENAI_DEPLOYMENT_NAME_MAP ("model=deployment,..."); the model's baseUrl is the fallback.
 */
export class AzureResponsesProvider : public IProvider {
public:
    AzureResponsesProvider(IHttpClient& http, ISleeper& sleeper, const IClock& clock, IExecutor& executor, const IEnvironment& environment)
        : m_http(http),
          m_sleeper(sleeper),
          m_clock(clock),
          m_executor(executor),
          m_environment(environment) {}

    std::string api() const override {
        return "azure-openai-responses";
    }

    std::shared_ptr<AssistantMessageStream> stream(const Model& model, const TranscriptContext& context, const StreamOptions& options) override {
        auto emitter = std::make_shared<AssistantStreamEmitter>(initialMessage(model));
        auto stream = emitter->stream();
        m_executor.submit([this, emitter, model, context, options]() { run(emitter, model, context, options); });
        return stream;
    }

private:
    void run(const std::shared_ptr<AssistantStreamEmitter>& emitter, const Model& model, const TranscriptContext& context, const StreamOptions& options) {
        if (!options.apiKey || options.apiKey->empty()) {
            emitter->error(StopReason::Error, "No API key for provider: " + model.provider);
            return;
        }
        const auto base = baseUrl(model, options);
        if (!base) {
            emitter->error(StopReason::Error, base.error().message);
            return;
        }
        RetryingHttpSender sender(m_http, m_sleeper, m_clock);
        SseRequestRunner runner(sender);
        ResponsesStreamReader reader(*emitter, model);
        runner.run(
            buildRequest(model, context, options, *options.apiKey, *base), model, options, *emitter,
            [&reader](const SseEvent& event) { return reader.handle(event); }, [&reader]() { return reader.finish(); },
            [this](const HttpResponse& response) { return m_formatter.formatOpenAi(response); });
    }

    AssistantMessage initialMessage(const Model& model) const {
        AssistantMessage message;
        message.api = "azure-openai-responses";
        message.provider = model.provider;
        message.model = model.id;
        message.stopReason = StopReason::Pending;
        message.timestamp = m_clock.nowMs();
        return message;
    }

    std::optional<std::string> envValue(const StreamOptions& options, const std::string& name) const {
        const auto scoped = options.env.find(name);
        if (scoped != options.env.end() && !scoped->second.empty()) {
            return scoped->second;
        }
        const auto process = m_environment.get(name);
        if (process && !process->empty()) {
            return process;
        }
        return std::nullopt;
    }

    std::string deploymentName(const Model& model, const StreamOptions& options) const {
        const auto map = envValue(options, "AZURE_OPENAI_DEPLOYMENT_NAME_MAP");
        if (!map) {
            return model.id;
        }
        std::size_t start = 0;
        while (start <= map->size()) {
            std::size_t end = map->find(',', start);
            end = end == std::string::npos ? map->size() : end;
            const std::string entry = trimmed(map->substr(start, end - start));
            const auto equals = entry.find('=');
            if (equals != std::string::npos && trimmed(entry.substr(0, equals)) == model.id) {
                const std::string deployment = trimmed(entry.substr(equals + 1));
                if (!deployment.empty()) {
                    return deployment;
                }
            }
            start = end + 1;
        }
        return model.id;
    }

    Result<std::string> baseUrl(const Model& model, const StreamOptions& options) const {
        std::string resolved = trimmed(envValue(options, "AZURE_OPENAI_BASE_URL").value_or(""));
        if (resolved.empty()) {
            if (const auto resource = envValue(options, "AZURE_OPENAI_RESOURCE_NAME")) {
                resolved = "https://" + *resource + ".openai.azure.com/openai/v1";
            }
        }
        if (resolved.empty()) {
            resolved = model.baseUrl;
        }
        if (resolved.empty()) {
            return std::unexpected(Error{"config", "Azure OpenAI base URL is required. Set AZURE_OPENAI_BASE_URL or "
                                                   "AZURE_OPENAI_RESOURCE_NAME, or give the model a baseUrl."});
        }
        const std::string normalized = normalizeBaseUrl(resolved);
        if (normalized.empty()) {
            return std::unexpected(Error{"config", "Invalid Azure OpenAI base URL: " + resolved});
        }
        return normalized;
    }

    std::string normalizeBaseUrl(const std::string& raw) const {
        std::string url = trimmed(raw);
        while (!url.empty() && url.back() == '/') {
            url.pop_back();
        }
        const auto scheme = url.find("://");
        if (scheme == std::string::npos) {
            return "";
        }
        const auto hostEnd = url.find_first_of("/?#", scheme + 3);
        const std::string host = url.substr(scheme + 3, hostEnd == std::string::npos ? std::string::npos : hostEnd - scheme - 3);
        std::string path = hostEnd != std::string::npos && url[hostEnd] == '/' ? url.substr(hostEnd) : "";
        const auto queryAt = path.find_first_of("?#");
        if (queryAt != std::string::npos) {
            path.resize(queryAt);
        }
        while (!path.empty() && path.back() == '/') {
            path.pop_back();
        }
        const bool azureHost = endsWith(host, ".openai.azure.com") || endsWith(host, ".cognitiveservices.azure.com") ||
                               endsWith(host, ".ai.azure.com");
        if (azureHost && (path.empty() || path == "/openai" || path == "/openai/v1/responses")) {
            return url.substr(0, scheme + 3) + host + "/openai/v1";
        }
        return url.substr(0, scheme + 3) + host + path;
    }

    std::string trimmed(const std::string& text) const {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    bool endsWith(const std::string& text, const std::string& suffix) const {
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    Json buildBody(const Model& model, const TranscriptContext& context, const StreamOptions& options) const {
        // Azure sends tool strictness by default and takes no prompt cache retention fields.
        Model azure = model;
        if (!azure.compat.is_object()) {
            azure.compat = Json::object();
        }
        if (!azure.compat.contains("supportsStrictMode")) {
            azure.compat["supportsStrictMode"] = true;
        }
        Json body = m_builder.build(azure, context, options, m_clock.nowMs(),
                                    {"openai", "openai-codex", "opencode", "azure-openai-responses"});
        body["model"] = deploymentName(model, options);
        body.erase("prompt_cache_retention");
        body.erase("prompt_cache_options");
        return body;
    }

    HttpRequest buildRequest(const Model& model, const TranscriptContext& context, const StreamOptions& options, const std::string& apiKey, const std::string& url) const {
        Json body = buildBody(model, context, options);
        if (options.onPayload) {
            if (auto replaced = options.onPayload(body, model)) {
                body = std::move(*replaced);
            }
        }
        HttpRequest request;
        request.method = "POST";
        request.url = url + "/responses?api-version=" + envValue(options, "AZURE_OPENAI_API_VERSION").value_or("v1");
        HttpHeaders headers = {{"User-Agent", "pi"}};
        headers = m_headers.merge(headers, model.headers);
        headers = m_headers.merge(headers, options.headers);
        m_headers.set(headers, "content-type", "application/json");
        m_headers.set(headers, "accept", "application/json");
        m_headers.set(headers, "api-key", apiKey);
        request.headers = headers;
        request.body = m_writer.compact(body);
        if (options.timeoutMs) {
            request.idleTimeout = std::chrono::milliseconds(*options.timeoutMs);
        }
        return request;
    }

    IHttpClient& m_http;
    ISleeper& m_sleeper;
    const IClock& m_clock;
    IExecutor& m_executor;
    const IEnvironment& m_environment;
    ResponsesRequestBuilder m_builder;
    HeaderMerger m_headers;
    JsonWriter m_writer;
    ProviderErrorFormatter m_formatter;
};
