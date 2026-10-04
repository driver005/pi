export module pi.support.sse_request_runner;

import std;
export import pi.support.assistant_stream_emitter;
export import pi.support.retrying_http_sender;
export import pi.support.sse_parser;
export import pi.types.model;
export import pi.types.stream_options;

/**
 * The part of a streaming provider call that is the same for every wire API: send the request
 * with retries, start the stream on a 2xx response, feed the body through the SSE parser into
 * the provider's event handler, and turn every failure into a terminal Error event.
 * Runs on the calling thread; providers invoke it from an executor task.
 */
export class SseRequestRunner {
public:
    using EventHandler = std::function<Result<void>(const SseEvent&)>;
    using Finisher = std::function<Result<void>()>;
    using ErrorFormatter = std::function<std::string(const HttpResponse&)>;

    explicit SseRequestRunner(RetryingHttpSender& sender)
        : m_sender(sender) {}

    /**
     * handler sees each SSE event in order; an Error aborts the request. finish runs after the
     * body ended and must emit the Done event. formatError renders non-2xx responses.
     */
    void run(HttpRequest request, const Model& model, const StreamOptions& options, AssistantStreamEmitter& emitter, const EventHandler& handler, const Finisher& finish, const ErrorFormatter& formatError) {
        auto local = std::make_shared<AbortSignal>();
        std::uint64_t listener = 0;
        if (options.signal) {
            listener = options.signal->onAbort([local]() { local->abort(); });
        }
        request.signal = local;
        SseParser parser;
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
            failure = dispatch(parser.feed(chunk), handler, model, options);
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
        if (auto tail = dispatch(parser.finish(), handler, model, options)) {
            fail(emitter, options, tail->message);
            return;
        }
        if (auto done = finish(); !done) {
            fail(emitter, options, done.error().message);
        }
    }

private:
    void fail(AssistantStreamEmitter& emitter, const StreamOptions& options, const std::string& message) const {
        const bool aborted = options.signal && options.signal->aborted();
        emitter.error(aborted ? StopReason::Aborted : StopReason::Error,
                      aborted ? "Request was aborted" : message);
    }

    std::optional<Error> dispatch(const std::vector<SseEvent>& events, const EventHandler& handler, const Model& model, const StreamOptions& options) const {
        for (const auto& event : events) {
            if (options.onStreamEvent) {
                const Json data = Json::parse(event.data, nullptr, false);
                if (!data.is_discarded()) {
                    options.onStreamEvent(data, model);
                }
            }
            auto result = handler(event);
            if (!result) {
                return result.error();
            }
        }
        return std::nullopt;
    }

    RetryingHttpSender& m_sender;
};
