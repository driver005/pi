export module pi.ai.codex_provider;

import std;
export import pi.platform.i_base64_codec;
export import pi.platform.i_clock;
export import pi.platform.i_executor;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
export import pi.provider.i_provider;
export import pi.support.assistant_stream_emitter;
export import pi.support.codex_request_builder;
export import pi.support.codex_stream_reader;
export import pi.support.header_merger;
export import pi.support.json_writer;
export import pi.support.retrying_http_sender;
export import pi.support.sse_request_runner;
export import pi.types.json;

/**
 * ChatGPT Codex backend (openai-codex-responses): POST {baseUrl}/codex/responses with SSE
 * streaming, authenticated with a ChatGPT access token whose account id goes into
 * `chatgpt-account-id`. Port of api/openai-codex-responses.ts for the SSE transport; the
 * WebSocket transport and zstd request compression are not ported.
 */
export class CodexProvider : public IProvider {
public:
    CodexProvider(IHttpClient& http, ISleeper& sleeper, const IClock& clock, IExecutor& executor, const IBase64Codec& base64)
        : m_http(http),
          m_sleeper(sleeper),
          m_clock(clock),
          m_executor(executor),
          m_base64(base64) {}

    std::string api() const override {
        return "openai-codex-responses";
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
        const auto account = accountId(*options.apiKey);
        if (!account) {
            emitter->error(StopReason::Error, account.error().message);
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
        request.url = codexUrl(model.baseUrl);
        request.headers = requestHeaders(model, options, *account);
        request.body = m_writer.compact(body);
        if (options.timeoutMs) {
            request.idleTimeout = std::chrono::milliseconds(*options.timeoutMs);
        }
        RetryingHttpSender sender(m_http, m_sleeper, m_clock);
        SseRequestRunner runner(sender);
        CodexStreamReader reader(*emitter, model);
        runner.run(
            std::move(request), model, options, *emitter, [&reader](const SseEvent& event) { return reader.handle(event); },
            [&reader]() { return reader.finish(); }, [this](const HttpResponse& response) { return formatError(response); });
    }

    AssistantMessage initialMessage(const Model& model) const {
        AssistantMessage message;
        message.api = "openai-codex-responses";
        message.provider = model.provider;
        message.model = model.id;
        message.stopReason = StopReason::Pending;
        message.timestamp = m_clock.nowMs();
        return message;
    }

    Result<std::string> accountId(const std::string& token) const {
        const auto first = token.find('.');
        const auto second = first == std::string::npos ? std::string::npos : token.find('.', first + 1);
        if (second == std::string::npos || token.find('.', second + 1) != std::string::npos) {
            return std::unexpected(Error{"invalid_token", "Failed to extract accountId from token"});
        }
        const auto payload = m_base64.decode(token.substr(first + 1, second - first - 1));
        const Json claims = payload ? Json::parse(*payload, nullptr, false) : Json();
        if (claims.is_object() && claims.contains("https://api.openai.com/auth") &&
            claims["https://api.openai.com/auth"].is_object()) {
            const Json& auth = claims["https://api.openai.com/auth"];
            if (auth.contains("chatgpt_account_id") && auth["chatgpt_account_id"].is_string() &&
                !auth["chatgpt_account_id"].get<std::string>().empty()) {
                return auth["chatgpt_account_id"].get<std::string>();
            }
        }
        return std::unexpected(Error{"invalid_token", "Failed to extract accountId from token"});
    }

    std::string codexUrl(const std::string& baseUrl) const {
        std::string base = baseUrl.find_first_not_of(" \t") == std::string::npos ? "https://chatgpt.com/backend-api" : baseUrl;
        while (!base.empty() && base.back() == '/') {
            base.pop_back();
        }
        if (base.ends_with("/codex/responses")) {
            return base;
        }
        return base.ends_with("/codex") ? base + "/responses" : base + "/codex/responses";
    }

    HttpHeaders requestHeaders(const Model& model, const StreamOptions& options, const std::string& account) const {
        HttpHeaders headers = m_headers.merge({}, model.headers);
        headers = m_headers.merge(headers, options.headers);
        m_headers.set(headers, "Authorization", "Bearer " + *options.apiKey);
        m_headers.set(headers, "chatgpt-account-id", account);
        m_headers.set(headers, "originator", "pi");
        m_headers.set(headers, "User-Agent", "pi");
        m_headers.set(headers, "OpenAI-Beta", "responses=experimental");
        m_headers.set(headers, "accept", "text/event-stream");
        m_headers.set(headers, "content-type", "application/json");
        if (options.sessionId && options.cacheRetention.value_or("short") != "none") {
            const std::string session = clampSession(*options.sessionId);
            m_headers.set(headers, "session-id", session);
            m_headers.set(headers, "x-client-request-id", session);
        }
        return headers;
    }

    std::string clampSession(const std::string& id) const {
        std::size_t points = 0;
        for (std::size_t i = 0; i < id.size(); ++i) {
            if ((static_cast<unsigned char>(id[i]) & 0xC0) != 0x80 && ++points > 64) {
                return id.substr(0, i);
            }
        }
        return id;
    }

    std::string formatError(const HttpResponse& response) const {
        std::string message = response.body.empty() ? "Request failed" : response.body;
        std::string friendly;
        const Json parsed = Json::parse(response.body, nullptr, false);
        if (parsed.is_object() && parsed.contains("error") && parsed["error"].is_object()) {
            const Json& error = parsed["error"];
            const auto text = [&](const std::string& key) {
                return error.contains(key) && error[key].is_string() ? error[key].get<std::string>() : std::string();
            };
            std::string code = text("code").empty() ? text("type") : text("code");
            std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool limited = code.find("usage_limit_reached") != std::string::npos ||
                                 code.find("usage_not_included") != std::string::npos ||
                                 code.find("rate_limit_exceeded") != std::string::npos || response.status == 429;
            if (limited) {
                std::string plan = text("plan_type");
                std::transform(plan.begin(), plan.end(), plan.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::string when;
                if (error.contains("resets_at") && error["resets_at"].is_number()) {
                    const double remainingMs = error["resets_at"].get<double>() * 1000.0 - static_cast<double>(m_clock.nowMs());
                    when = " Try again in ~" + std::to_string(std::max<std::int64_t>(0, std::llround(remainingMs / 60000.0))) + " min.";
                }
                friendly = "You have hit your ChatGPT usage limit" + (plan.empty() ? "" : " (" + plan + " plan)") + "." + when;
            }
            message = !text("message").empty() ? text("message") : !friendly.empty() ? friendly : message;
        }
        return !friendly.empty() ? friendly : message;
    }

    IHttpClient& m_http;
    ISleeper& m_sleeper;
    const IClock& m_clock;
    IExecutor& m_executor;
    const IBase64Codec& m_base64;
    CodexRequestBuilder m_builder;
    HeaderMerger m_headers;
    JsonWriter m_writer;
};
