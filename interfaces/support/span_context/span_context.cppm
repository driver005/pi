export module pi.support.span_context;

import std;
export import pi.telemetry.i_telemetry_context;

/** A running span seen as a context, for code that starts spans through an ITelemetryContext: new spans become its children. */
export class SpanContext : public ITelemetryContext {
public:
    explicit SpanContext(ITelemetrySpan& span)
        : m_span(span) {}

    Result<void> startSpan(const SpanOptions& options, const std::function<Result<void>(ITelemetrySpan&)>& callback) override {
        return m_span.startChild(options, callback);
    }

private:
    ITelemetrySpan& m_span;
};
