#pragma once

#include <memory>
#include <string>

#include "interfaces/support/event_stream/event_stream.h"
#include "interfaces/types/assistant_message/assistant_message.h"
#include "interfaces/types/assistant_message_event/assistant_message_event.h"
#include "interfaces/types/model/model.h"
#include "interfaces/types/stream_options/stream_options.h"
#include "interfaces/types/transcript_context/transcript_context.h"

/** Stream of events ending with the final AssistantMessage as its result. */
using AssistantMessageStream = EventStream<AssistantMessageEvent, AssistantMessage>;

/**
 * One LLM wire protocol (anthropic-messages, openai-completions, ...). Implementations never
 * throw and never fail synchronously: request, auth and transport failures are reported through
 * the returned stream as an Error event carrying a message with stopReason error|aborted.
 */
class IProvider {
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
