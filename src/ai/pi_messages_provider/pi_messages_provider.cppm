export module pi.ai.pi_messages_provider;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_executor;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
export import pi.provider.i_provider;
export import pi.support.assistant_stream_emitter;
export import pi.support.header_merger;
export import pi.support.json_writer;
export import pi.support.message_codec;
export import pi.support.pi_messages_stream_reader;
export import pi.support.retrying_http_sender;
export import pi.support.sse_request_runner;
export import pi.types.json;

/**
 * pi-messages: pi's own message protocol. One POST of {model, context, options} to
 * {baseUrl}/messages answers with an SSE stream of serialized assistant-message events. This is
 * the protocol of the Radius gateway; any backend speaking it works through a models.json
 * provider with `"api": "pi-messages"`. Port of api/pi-messages.ts (the `debug` query option is
 * not exposed).
 */
export class PiMessagesProvider : public IProvider {
public:
    PiMessagesProvider(IHttpClient& http, ISleeper& sleeper, const IClock& clock, IExecutor& executor)
        : m_http(http),
          m_sleeper(sleeper),
          m_clock(clock),
          m_executor(executor) {}

    std::string api() const override {
        return "pi-messages";
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
            emitter->error(StopReason::Error, "No API key provided for provider \"" + model.provider + "\"");
            return;
        }
        std::string base = model.baseUrl;
        while (!base.empty() && base.back() == '/') {
            base.pop_back();
        }
        const std::string url = base + "/messages";
        Json body = payload(model, context, options);
        if (options.onPayload) {
            if (auto replaced = options.onPayload(body, model)) {
                body = std::move(*replaced);
            }
        }
        HttpRequest request;
        request.method = "POST";
        request.url = url;
        HttpHeaders headers = {{"authorization", "Bearer " + *options.apiKey},
                               {"accept", "text/event-stream"},
                               {"content-type", "application/json"}};
        request.headers = m_headers.merge(headers, options.headers);
        request.body = m_writer.compact(body);
        if (options.timeoutMs) {
            request.idleTimeout = std::chrono::milliseconds(*options.timeoutMs);
        }
        RetryingHttpSender sender(m_http, m_sleeper, m_clock);
        SseRequestRunner runner(sender);
        PiMessagesStreamReader reader(*emitter, m_clock);
        runner.run(
            std::move(request), model, options, *emitter, [&reader](const SseEvent& event) { return reader.handle(event); },
            [&reader, &model]() { return reader.finish(model.provider); },
            [&](const HttpResponse& response) { return formatError(*emitter, model, url, response); });
    }

    AssistantMessage initialMessage(const Model& model) const {
        AssistantMessage message;
        message.api = model.api;
        message.provider = model.provider;
        message.model = model.id;
        message.stopReason = StopReason::Pending;
        message.timestamp = m_clock.nowMs();
        return message;
    }

    Json payload(const Model& model, const TranscriptContext& context, const StreamOptions& options) const {
        Json settings = Json::object();
        if (options.temperature) {
            settings["temperature"] = *options.temperature;
        }
        if (options.maxTokens) {
            settings["maxTokens"] = *options.maxTokens;
        }
        if (options.reasoning != ThinkingLevel::Off) {
            settings["reasoning"] = m_codec.thinkingLevelName(options.reasoning);
        }
        if (const auto retention = cacheRetention(options)) {
            settings["cacheRetention"] = *retention;
        }
        if (options.sessionId) {
            settings["sessionId"] = *options.sessionId;
        }
        if (options.toolChoice) {
            settings["toolChoice"] = *options.toolChoice;
        }
        return Json{{"model", model.id},
                    {"context", Json{{"messages", m_codec.toJson(context.messages)}}},
                    {"options", std::move(settings)}};
    }

    std::optional<std::string> cacheRetention(const StreamOptions& options) const {
        if (options.cacheRetention) {
            return options.cacheRetention;
        }
        // Backend defaults apply when unset; only the legacy env opt-in is mapped.
        const auto env = options.env.find("PI_CACHE_RETENTION");
        return env != options.env.end() && env->second == "long" ? std::optional<std::string>("long") : std::nullopt;
    }

    std::string errorMessage(const HttpResponse& response, const Json& body) const {
        std::string suffix = response.body;
        std::string code;
        if (body.is_object() && body.contains("error") && body["error"].is_object()) {
            const Json& error = body["error"];
            if (error.contains("message") && error["message"].is_string()) {
                suffix = error["message"].get<std::string>();
            }
            if (error.contains("code") && error["code"].is_string()) {
                code = error["code"].get<std::string>();
            }
        }
        return std::to_string(response.status) + ": " + suffix + (code.empty() ? "" : " (" + code + ")");
    }

    std::string formatError(AssistantStreamEmitter& emitter, const Model& model, const std::string& url, const HttpResponse& response) const {
        const Json body = Json::parse(response.body, nullptr, false);
        const bool structured = body.is_object() && body.contains("error") && body["error"].is_object();
        const std::string message = errorMessage(response, body);
        Json details = Json{{"version", 1},
                            {"provider", model.provider},
                            {"model", model.id},
                            {"url", url},
                            {"status", response.status},
                            {"timestampMs", m_clock.nowMs()}};
        if (structured) {
            details["error"] = body["error"];
        } else {
            details["body"] = response.body.size() > 8192 ? response.body.substr(0, 8192) + "…" : response.body;
        }
        Json error = Json{{"name", "PiMessagesResponseError"}, {"message", message}};
        if (structured && body["error"].contains("code") && body["error"]["code"].is_string()) {
            error["code"] = body["error"]["code"];
        }
        emitter.message().diagnostics = Json::array({Json{{"type", "pi_messages_response_failure"},
                                                          {"timestamp", m_clock.nowMs()},
                                                          {"error", std::move(error)},
                                                          {"details", std::move(details)}}});
        return message;
    }

    IHttpClient& m_http;
    ISleeper& m_sleeper;
    const IClock& m_clock;
    IExecutor& m_executor;
    MessageCodec m_codec;
    HeaderMerger m_headers;
    JsonWriter m_writer;
};
