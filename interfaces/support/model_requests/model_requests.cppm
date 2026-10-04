module;

#include <cstdint>

export module pi.support.model_requests;

import std;
export import pi.provider.i_model_runtime;
export import pi.support.abort_signal;
export import pi.support.message_codec;
export import pi.support.thinking_level_resolver;
export import pi.support.virtual_model_names;
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

    /** Whether the model is virtual: a router picks the physical model of each request. */
    bool isVirtual(const Model& model) const {
        return m_virtual.isVirtual(model);
    }

    /** Type of the conversation entry that stores router state; its data is `{provider, modelId, state}`. */
    std::string virtualStateKind() const {
        return m_virtual.stateEntryType();
    }

    /** The router state last stored for `model` in the conversation's `entries`; null before the first. */
    Json virtualState(const Json& entries, const Model& model) const {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->value("kind", std::string()) != m_virtual.stateEntryType() || !it->contains("data") || !it->at("data").is_object()) {
                continue;
            }
            const Json& data = it->at("data");
            if (data.value("provider", std::string()) == model.provider && data.value("modelId", std::string()) == model.id) {
                return data.contains("state") ? data.at("state") : Json();
            }
        }
        return Json();
    }

    /**
     * Routes one request of a virtual model over JSON `messages`: `reason` is "user", "continuation", "retry" or "direct".
     * The answer is the physical model and thinking level (as a name); a new router state, when the router changed it, goes
     * to `newState`.
     */
    Result<Model> route(IModelRuntime& models, const Model& virtualModel, const std::string& thinkingLevel, const std::string& reason, const Json& messages, const Json& state, AbortSignal& signal, std::string& routedLevel,
                        std::optional<Json>& newState) const {
        auto typed = m_codec.messagesFromJson(messages);
        if (!typed) {
            return std::unexpected(typed.error());
        }
        VirtualResolveRequest request;
        request.model = virtualModel;
        request.thinkingLevel = m_codec.parseThinkingLevel(thinkingLevel).value_or(ThinkingLevel::Off);
        request.reason = reason;
        request.state = state;
        request.messages = std::move(*typed);
        request.signal = std::shared_ptr<AbortSignal>(std::shared_ptr<void>(), &signal);
        auto routed = models.resolveVirtual(request);
        if (!routed) {
            return std::unexpected(routed.error());
        }
        routedLevel = m_codec.thinkingLevelName(routed->thinkingLevel);
        newState = std::move(routed->state);
        return std::move(routed->model);
    }

    /**
     * The model whose limits (context window) apply to `messages`: `model` itself, or for a virtual one the physical model that
     * produced the latest successful response (the virtual model's declared limits before the first).
     */
    Model limitsModel(IModelRuntime* models, const Model& model, const Json& messages) const {
        if (models == nullptr || !m_virtual.isVirtual(model) || !messages.is_array()) {
            return model;
        }
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            const std::string stop = it->value("stopReason", std::string());
            if (it->value("role", std::string()) == "assistant" && stop != "error" && stop != "aborted") {
                if (auto physical = models->physicalModel(it->value("provider", std::string()), it->value("model", std::string()))) {
                    return *physical;
                }
                break;
            }
        }
        return model;
    }

    /** "user" when the newest message is the user's, else "continuation"; the JSON counterpart of a request's messages. */
    std::string routeReason(const Json& messages, bool retry) const {
        if (retry) {
            return "retry";
        }
        const bool user = messages.is_array() && !messages.empty() && messages.back().value("role", std::string()) == "user";
        return user ? "user" : "continuation";
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

    /** Fetches the current state of a deferred response and drains it: the final message, or another `deferred` one. */
    Result<Json> fetchDeferred(IModelRuntime& models, const Model& model, const Json& handle, const StreamOptions& options) const {
        auto typed = m_codec.handleFromJson(handle);
        if (!typed) {
            return std::unexpected(Error{"model_error", "The deferred handle is not an object"});
        }
        std::shared_ptr<AssistantMessageStream> stream = models.fetchDeferred(model, *typed, options);
        if (!stream) {
            return std::unexpected(Error{"no_model", "Model " + model.provider + "/" + model.id + " produced no stream"});
        }
        while (stream->next()) {
        }
        auto final = stream->result();
        if (!final) {
            return std::unexpected(Error{"model_error", "The model stream ended without a result"});
        }
        return m_codec.toJson(*final);
    }

    Result<void> cancelDeferred(IModelRuntime& models, const Model& model, const Json& handle, const StreamOptions& options) const {
        auto typed = m_codec.handleFromJson(handle);
        if (!typed) {
            return std::unexpected(Error{"model_error", "The deferred handle is not an object"});
        }
        return models.cancelDeferred(model, *typed, options);
    }

    Json toJson(const AssistantMessage& message) const {
        return m_codec.toJson(message);
    }

private:
    MessageCodec m_codec;
    VirtualModelNames m_virtual;
};
