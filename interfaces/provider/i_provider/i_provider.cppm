export module pi.provider.i_provider;

import std;
export import pi.support.event_stream;
export import pi.types.assistant_message;
export import pi.types.assistant_message_event;
export import pi.types.deferred_handle;
export import pi.types.model;
export import pi.types.result;
export import pi.types.stream_options;
export import pi.types.transcript_context;

/** Stream of events ending with the final AssistantMessage as its result. */
export using AssistantMessageStream = EventStream<AssistantMessageEvent, AssistantMessage>;

/**
 * One LLM wire protocol (anthropic-messages, openai-completions, ...). Implementations never
 * throw and never fail synchronously: request, auth and transport failures are reported through
 * the returned stream as an Error event carrying a message with stopReason error|aborted.
 */
export class IProvider {
public:
    virtual ~IProvider() = default;

    /** Wire API name this provider implements, matching Model::api. */
    virtual std::string api() const = 0;

    /**
     * Starts the request and returns immediately; events arrive from a background thread.
     * The context is already normalized (see TranscriptNormalizer).
     */
    virtual std::shared_ptr<AssistantMessageStream> stream(const Model& model,
                                                           const TranscriptContext& context,
                                                           const StreamOptions& options) = 0;

    /** Whether the wire API can continue a response asynchronously (the `deferred` stream option). */
    virtual bool supportsDeferred() const {
        return false;
    }

    /**
     * Fetches the current state of a deferred response: another `deferred` message while it is pending, then the final
     * message. Only called when supportsDeferred(); failures arrive as an Error event.
     */
    virtual std::shared_ptr<AssistantMessageStream> fetchDeferred(const Model&, const DeferredHandle&, const StreamOptions&) {
        return nullptr;
    }

    /** Cancels a deferred response. Only called when supportsDeferred(). */
    virtual Result<void> cancelDeferred(const Model&, const DeferredHandle&, const StreamOptions&) {
        return std::unexpected(Error{"provider", "The provider does not support deferred responses"});
    }
};
