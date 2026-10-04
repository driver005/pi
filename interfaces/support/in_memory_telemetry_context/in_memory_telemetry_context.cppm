export module pi.support.in_memory_telemetry_context;

import std;
export import pi.support.in_memory_telemetry_recorder;
export import pi.telemetry.i_telemetry_context;
import pi.support.in_memory_telemetry_span;

/**
 * Backend-neutral reference context that records spans in process memory. Create a fresh instance to isolate tests or
 * independent recording scopes.
 */
export class InMemoryTelemetryContext : public ITelemetryContext {
public:
    Result<void> startSpan(const SpanOptions& options, const std::function<Result<void>(ITelemetrySpan&)>& callback) override {
        InMemoryTelemetrySpan root(m_recorder, 0);
        return root.startChild(options, callback);
    }

    /** Detached snapshots in span-start order. */
    std::vector<RecordedTelemetrySpan> getSpans() const {
        return m_recorder.spans();
    }

private:
    InMemoryTelemetryRecorder m_recorder;
};
