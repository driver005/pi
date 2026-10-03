module;

#include <cstdint>

export module pi.support.summarization_retry_reporter;

import std;
export import pi.session.i_session_event_sink;
export import pi.types.retry_callbacks;

/** Turns the retry progress of a summarization call into session events. */
export class SummarizationRetryReporter {
public:
    explicit SummarizationRetryReporter(ISessionEventSink& sink)
        : m_sink(sink) {}

    /** source is "compaction" (with its reason) or "branchSummary". */
    RetryCallbacks callbacks(const std::string& source, const std::string& reason) const {
        ISessionEventSink& sink = m_sink;
        RetryCallbacks out;
        out.onRetryScheduled = [&sink](int attempt, int maxAttempts, std::int64_t delayMs, const std::string& error) {
            AgentSessionEvent event;
            event.type = SessionEventType::SummarizationRetryScheduled;
            event.attempt = attempt;
            event.maxAttempts = maxAttempts;
            event.delayMs = delayMs;
            event.errorMessage = error;
            sink.emit(event);
        };
        out.onRetryAttemptStart = [&sink, source, reason] {
            AgentSessionEvent event;
            event.type = SessionEventType::SummarizationRetryAttemptStart;
            event.source = source;
            event.reason = reason;
            sink.emit(event);
        };
        out.onRetryFinished = [&sink](bool, int, const std::optional<std::string>&) {
            AgentSessionEvent event;
            event.type = SessionEventType::SummarizationRetryFinished;
            sink.emit(event);
        };
        return out;
    }

private:
    ISessionEventSink& m_sink;
};
