module;

#include <cstdint>

export module pi.support.event_stream_request_runner;

import std;
export import pi.support.assistant_stream_emitter;
export import pi.support.aws_event_stream_parser;
export import pi.support.retrying_http_sender;
export import pi.types.model;
export import pi.types.stream_options;

/**
 * The AWS event stream counterpart of SseRequestRunner: send the request with retries, start the
 * stream on a 2xx response, feed the binary body through the event stream parser into the
 * provider's message handler and turn every failure into a terminal Error event.
 */
export class EventStreamRequestRunner {
public:
    using MessageHandler = std::function<Result<void>(const AwsEventStreamMessage&)>;
    using Finisher = std::function<Result<void>()>;
    using ErrorFormatter = std::function<std::string(const HttpResponse&)>;

    explicit EventStreamRequestRunner(RetryingHttpSender& sender);

    void run(HttpRequest request, const Model& model, const StreamOptions& options, AssistantStreamEmitter& emitter,
             const MessageHandler& handler, const Finisher& finish, const ErrorFormatter& formatError);

private:
    void fail(AssistantStreamEmitter& emitter, const StreamOptions& options, const std::string& message) const;
    std::optional<Error> dispatch(const std::vector<AwsEventStreamMessage>& messages, const MessageHandler& handler) const;

    RetryingHttpSender& m_sender;
};

EventStreamRequestRunner::EventStreamRequestRunner(RetryingHttpSender& sender) : m_sender(sender) {}

void EventStreamRequestRunner::fail(AssistantStreamEmitter& emitter, const StreamOptions& options,
                                    const std::string& message) const {
    const bool aborted = options.signal && options.signal->aborted();
    emitter.error(aborted ? StopReason::Aborted : StopReason::Error, aborted ? "Request was aborted" : message);
}

std::optional<Error> EventStreamRequestRunner::dispatch(const std::vector<AwsEventStreamMessage>& messages,
                                                        const MessageHandler& handler) const {
    for (const auto& message : messages) {
        auto result = handler(message);
        if (!result) {
            return result.error();
        }
    }
    return std::nullopt;
}

void EventStreamRequestRunner::run(HttpRequest request, const Model& model, const StreamOptions& options,
                                   AssistantStreamEmitter& emitter, const MessageHandler& handler,
                                   const Finisher& finish, const ErrorFormatter& formatError) {
    auto local = std::make_shared<AbortSignal>();
    std::uint64_t listener = 0;
    if (options.signal) {
        listener = options.signal->onAbort([local]() { local->abort(); });
    }
    request.signal = local;
    AwsEventStreamParser parser;
    std::optional<Error> failure;
    request.onResponse = [&](int status, const HttpHeaders& headers) {
        if (status < 200 || status >= 300) {
            return;
        }
        if (options.onResponse) {
            ProviderResponse response;
            response.status = status;
            response.headers = headers;
            options.onResponse(response, model);
        }
        emitter.start();
    };
    request.onBody = [&](std::string_view chunk) {
        if (failure) {
            return;
        }
        auto messages = parser.feed(chunk);
        failure = messages ? dispatch(*messages, handler) : std::optional<Error>(messages.error());
        if (failure) {
            local->abort();
        }
    };

    auto response = m_sender.send(request, options.maxRetries.value_or(0), options.maxRetryDelayMs);
    if (options.signal) {
        options.signal->removeListener(listener);
    }
    if (failure) {
        fail(emitter, options, failure->message);
        return;
    }
    if (!response) {
        fail(emitter, options, response.error().message);
        return;
    }
    if (response->status < 200 || response->status >= 300) {
        fail(emitter, options, formatError(response.value()));
        return;
    }
    if (auto tail = parser.finish(); !tail) {
        fail(emitter, options, tail.error().message);
        return;
    }
    if (auto done = finish(); !done) {
        fail(emitter, options, done.error().message);
    }
}
