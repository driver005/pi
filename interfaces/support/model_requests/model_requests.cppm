module;

#include <cstdint>

export module pi.support.model_requests;

import std;
export import pi.provider.i_model_runtime;
export import pi.support.abort_signal;
export import pi.support.message_codec;
export import pi.types.json;
export import pi.types.result;
export import pi.types.stream_options;

/**
 * Turns the durable harness's JSON view of a request (messages, curated stream options, thinking level) into a typed
 * model call, and typed responses back into JSON. The harness stores everything as JSON; the providers take typed
 * messages.
 */
export class ModelRequests {
public:
    /** The catalog model of a `{provider, modelId}` reference; `no_model` when the harness has none or it is unknown. */
    Result<Model> find(IModelRuntime* models, const Json& ref) const {
        if (models == nullptr) {
            return std::unexpected(Error{"no_model", "No model runtime is configured"});
        }
        auto model = models->find(ref.at("provider").get<std::string>(), ref.at("modelId").get<std::string>());
        if (!model) {
            return std::unexpected(Error{"no_model", "Model " + ref.at("provider").get<std::string>() + "/" +
                                                         ref.at("modelId").get<std::string>() + " is not available"});
        }
        return *model;
    }

    /**
     * The request options for the curated settings object (`transport`, `timeoutMs`, `maxRetries`, `maxRetryDelayMs`,
     * `headers`, `metadata`, `cacheRetention`, `deferred`), a thinking level and the invocation's signal.
     */
    StreamOptions options(const Json& stream, const std::string& thinkingLevel, AbortSignal& signal) const {
        StreamOptions options;
        // A non-owning handle: the invocation owns its signal and outlives the request.
        options.signal = std::shared_ptr<AbortSignal>(std::shared_ptr<void>(), &signal);
        if (stream.contains("transport")) {
            options.transport = stream.at("transport").get<std::string>();
        }
        if (stream.contains("timeoutMs")) {
            options.timeoutMs = stream.at("timeoutMs").get<std::int64_t>();
        }
        if (stream.contains("maxRetries")) {
            options.maxRetries = stream.at("maxRetries").get<int>();
        }
        if (stream.contains("maxRetryDelayMs")) {
            options.maxRetryDelayMs = stream.at("maxRetryDelayMs").get<std::int64_t>();
        }
        if (stream.contains("headers")) {
            for (const auto& header : stream.at("headers").items()) {
                options.headers.emplace_back(header.key(), header.value().get<std::string>());
            }
        }
        if (stream.contains("metadata")) {
            options.metadata = stream.at("metadata");
        }
        if (stream.contains("cacheRetention")) {
            options.cacheRetention = stream.at("cacheRetention").get<std::string>();
        }
        if (stream.contains("deferred")) {
            options.deferred = stream.at("deferred");
        }
        if (auto level = m_codec.parseThinkingLevel(thinkingLevel)) {
            options.reasoning = *level;
        }
        return options;
    }

    /** Opens a stream for JSON `messages`; the stream ends with the final message or an error event. */
    Result<std::shared_ptr<AssistantMessageStream>> open(IModelRuntime& models, const Model& model, const Json& messages,
                                                         const StreamOptions& options) const {
        auto typed = m_codec.messagesFromJson(messages);
        if (!typed) {
            return std::unexpected(typed.error());
        }
        std::shared_ptr<AssistantMessageStream> stream = models.stream(model, TranscriptContext{std::move(*typed)}, options);
        if (!stream) {
            return std::unexpected(Error{"no_model", "Model " + model.provider + "/" + model.id + " produced no stream"});
        }
        return stream;
    }

    /** Opens a stream, drains it and returns the final message as JSON. */
    Result<Json> complete(IModelRuntime& models, const Model& model, const Json& messages, const StreamOptions& options) const {
        auto stream = open(models, model, messages, options);
        if (!stream) {
            return std::unexpected(stream.error());
        }
        while ((*stream)->next()) {
        }
        auto final = (*stream)->result();
        if (!final) {
            return std::unexpected(Error{"model_error", "The model stream ended without a result"});
        }
        return m_codec.toJson(*final);
    }

    Json toJson(const AssistantMessage& message) const {
        return m_codec.toJson(message);
    }

private:
    MessageCodec m_codec;
};
