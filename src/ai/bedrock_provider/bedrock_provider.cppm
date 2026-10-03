export module pi.ai.bedrock_provider;

import std;
export import pi.platform.i_base64_codec;
export import pi.platform.i_clock;
export import pi.platform.i_crypto;
export import pi.platform.i_environment;
export import pi.platform.i_executor;
export import pi.platform.i_file_system;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
export import pi.provider.i_provider;
export import pi.support.assistant_stream_emitter;
export import pi.support.aws_sigv4_signer;
export import pi.support.bedrock_connection_resolver;
export import pi.support.bedrock_request_builder;
export import pi.support.bedrock_stream_reader;
export import pi.support.event_stream_request_runner;
export import pi.support.header_merger;
export import pi.support.json_writer;
export import pi.support.retrying_http_sender;
export import pi.types.json;

/**
 * Amazon Bedrock (bedrock-converse-stream): POST /model/{id}/converse-stream answered with an AWS
 * event stream. Requests carry an AWS Signature V4 or a Bedrock API key bearer token. Port of
 * api/bedrock-converse-stream.ts without the AWS SDK: SSO, assume-role and instance metadata
 * credentials are not supported (see BedrockConnectionResolver).
 */
export class BedrockProvider : public IProvider {
public:
    BedrockProvider(IHttpClient& http, ISleeper& sleeper, const IClock& clock, IExecutor& executor, const IEnvironment& environment, IFileSystem& files, ICrypto& crypto, const IBase64Codec& base64)
        : m_http(http),
          m_sleeper(sleeper),
          m_clock(clock),
          m_executor(executor),
          m_environment(environment),
          m_base64(base64),
          m_connections(environment, files),
          m_builder(base64),
          m_signer(crypto) {}

    std::string api() const override {
        return "bedrock-converse-stream";
    }

    std::shared_ptr<AssistantMessageStream> stream(const Model& model, const TranscriptContext& context, const StreamOptions& options) override {
        auto emitter = std::make_shared<AssistantStreamEmitter>(initialMessage(model));
        auto stream = emitter->stream();
        m_executor.submit([this, emitter, model, context, options]() { run(emitter, model, context, options); });
        return stream;
    }

private:
    void run(const std::shared_ptr<AssistantStreamEmitter>& emitter, const Model& model, const TranscriptContext& context, const StreamOptions& rawOptions) {
        const StreamOptions options = withProcessEnvironment(rawOptions);
        auto connection = m_connections.resolve(model, options.apiKey, options.env);
        if (!connection) {
            emitter->error(StopReason::Error, connection.error().message);
            return;
        }
        auto built = m_builder.build(model, context, options, connection->region, m_clock.nowMs());
        if (!built) {
            emitter->error(StopReason::Error, built.error().message);
            return;
        }
        Json body = std::move(*built);
        if (options.onPayload) {
            if (auto replaced = options.onPayload(body, model)) {
                body = std::move(*replaced);
            }
        }
        RetryingHttpSender sender(m_http, m_sleeper, m_clock);
        EventStreamRequestRunner runner(sender);
        BedrockStreamReader reader(*emitter, model, m_base64);
        runner.run(
            buildRequest(model, body, options, *connection), model, options, *emitter,
            [&reader](const AwsEventStreamMessage& message) { return reader.handle(message); },
            [&reader]() { return reader.finish(); },
            [&](const HttpResponse& response) { return formatError(*emitter, response); });
    }

    AssistantMessage initialMessage(const Model& model) const {
        AssistantMessage message;
        message.api = "bedrock-converse-stream";
        message.provider = model.provider;
        message.model = model.id;
        message.stopReason = StopReason::Pending;
        message.timestamp = m_clock.nowMs();
        return message;
    }

    StreamOptions withProcessEnvironment(const StreamOptions& options) const {
        StreamOptions merged = options;
        for (const std::string name : {"PI_CACHE_RETENTION", "AWS_BEDROCK_FORCE_CACHE"}) {
            const auto value = m_environment.get(name);
            if (value && !merged.env.contains(name)) {
                merged.env[name] = *value;
            }
        }
        return merged;
    }

    HttpRequest buildRequest(const Model& model, const Json& body, const StreamOptions& options, const BedrockConnection& connection) const {
        const std::string endpoint = connection.endpoint;
        const auto scheme = endpoint.find("://");
        const auto pathStart = endpoint.find('/', scheme == std::string::npos ? 0 : scheme + 3);
        const std::string host = endpoint.substr(scheme == std::string::npos ? 0 : scheme + 3,
                                                 pathStart == std::string::npos ? std::string::npos
                                                                                : pathStart - (scheme == std::string::npos ? 0 : scheme + 3));
        const std::string prefix = pathStart == std::string::npos ? "" : endpoint.substr(pathStart);
        const std::string path = prefix + "/model/" + m_signer.encode(model.id, false) + "/converse-stream";
        HttpHeaders headers = {{"content-type", "application/json"}};
        for (const auto& header : options.headers) {
            if (header.second && !reservedHeader(header.first)) {
                m_headers.set(headers, header.first, *header.second);
            }
        }
        HttpRequest request;
        request.method = "POST";
        request.url = endpoint + "/model/" + m_signer.encode(model.id, false) + "/converse-stream";
        request.body = m_writer.compact(body);
        if (connection.bearerToken) {
            m_headers.set(headers, "Authorization", "Bearer " + *connection.bearerToken);
        } else if (connection.credentials) {
            Sigv4Request signing;
            signing.host = host;
            signing.path = path;
            signing.headers = headers;
            signing.body = request.body;
            signing.region = connection.region;
            signing.service = "bedrock";
            signing.credentials = *connection.credentials;
            for (const auto& added : m_signer.sign(signing, m_clock.nowMs())) {
                m_headers.set(headers, added.first, added.second);
            }
        }
        m_headers.set(headers, "accept", "application/vnd.amazon.eventstream");
        request.headers = headers;
        if (options.timeoutMs) {
            request.idleTimeout = std::chrono::milliseconds(*options.timeoutMs);
        }
        return request;
    }

    bool reservedHeader(const std::string& name) const {
        std::string lowered = name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowered.starts_with("x-amz-") || lowered == "authorization" || lowered == "host";
    }

    std::string formatError(AssistantStreamEmitter& emitter, const HttpResponse& response) const {
        const Json body = Json::parse(response.body, nullptr, false);
        std::string core = response.body;
        if (body.is_object()) {
            for (const std::string key : {"message", "Message"}) {
                if (body.contains(key) && body[key].is_string()) {
                    core = body[key].get<std::string>();
                    break;
                }
            }
        }
        const std::string name = errorName(response, body);
        std::string text = name.empty() ? std::to_string(response.status) + ": " + core : readableName(name) + ": " + core;
        std::string lowered = core;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered.find("data retention mode") != std::string::npos) {
            text += " See https://docs.aws.amazon.com/bedrock/latest/userguide/data-retention.html for supported data "
                    "retention modes.";
        }
        Json details = Json{{"status", response.status}};
        if (name.ends_with("Exception")) {
            details["errorCode"] = name;
        }
        if (const auto requestId = m_headers.find(response.headers, "x-amzn-requestid"); requestId && !requestId->empty() &&
                                                                                         requestId->size() <= 200) {
            details["requestId"] = *requestId;
        }
        emitter.message().diagnostics = Json::array({Json{{"type", "bedrock_response_failure"},
                                                          {"timestamp", m_clock.nowMs()},
                                                          {"details", std::move(details)}}});
        return text;
    }

    std::string errorName(const HttpResponse& response, const Json& body) const {
        std::string name = m_headers.find(response.headers, "x-amzn-errortype").value_or("");
        if (name.empty() && body.is_object() && body.contains("__type") && body["__type"].is_string()) {
            name = body["__type"].get<std::string>();
        }
        // The header carries "Name:http://..." and the body type may be namespaced ("ns#Name").
        name = name.substr(0, name.find(':'));
        const auto hash = name.rfind('#');
        return hash == std::string::npos ? name : name.substr(hash + 1);
    }

    std::string readableName(const std::string& name) const {
        if (name == "InternalServerException") {
            return "Internal server error";
        }
        if (name == "ModelStreamErrorException") {
            return "Model stream error";
        }
        if (name == "ValidationException") {
            return "Validation error";
        }
        if (name == "ThrottlingException") {
            return "Throttling error";
        }
        return name == "ServiceUnavailableException" ? "Service unavailable" : name;
    }

    IHttpClient& m_http;
    ISleeper& m_sleeper;
    const IClock& m_clock;
    IExecutor& m_executor;
    const IEnvironment& m_environment;
    const IBase64Codec& m_base64;
    BedrockConnectionResolver m_connections;
    BedrockRequestBuilder m_builder;
    AwsSigv4Signer m_signer;
    HeaderMerger m_headers;
    JsonWriter m_writer;
};
