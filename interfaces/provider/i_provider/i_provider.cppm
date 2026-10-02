export module pi.provider.i_provider;

import std;
export import pi.support.event_stream;
export import pi.types.assistant_message;
export import pi.types.assistant_message_event;
export import pi.types.model;
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
};
