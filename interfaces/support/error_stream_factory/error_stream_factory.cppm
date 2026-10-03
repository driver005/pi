export module pi.support.error_stream_factory;

import std;
export import pi.provider.i_provider;
export import pi.support.assistant_stream_emitter;
export import pi.types.model;

/** A stream that already ended in an Error event, for failures found before any request. */
export class ErrorStreamFactory {
public:
    std::shared_ptr<AssistantMessageStream> failed(const Model& model, const std::string& message, std::int64_t nowMs) const {
        AssistantMessage initial;
        initial.api = model.api;
        initial.provider = model.provider;
        initial.model = model.id;
        initial.stopReason = StopReason::Pending;
        initial.timestamp = nowMs;
        AssistantStreamEmitter emitter(std::move(initial));
        emitter.error(StopReason::Error, message);
        return emitter.stream();
    }
};
