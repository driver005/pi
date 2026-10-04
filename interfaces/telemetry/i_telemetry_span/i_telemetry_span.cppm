export module pi.telemetry.i_telemetry_span;

import std;
export import pi.types.json;
export import pi.types.result;
export import pi.types.span_options;
export import pi.types.span_status;

/**
 * A running span. Recording is passive: nothing here fails, calls after the span settled are ignored, and a null attribute value
 * is dropped. Port of TelemetrySpan in packages/telemetry; a span starts children through startChild instead of being a context
 * itself (see SpanContext for the adapter). A span is only valid inside the callback it was passed to (the TypeScript span object can outlive it, where calls after settlement are inert; here the reference ends with the callback).
 */
export class ITelemetrySpan {
public:
    virtual ~ITelemetrySpan() = default;

    /** Runs `callback` inside a child span; see ITelemetryContext::startSpan for the outcome rules. */
    virtual Result<void> startChild(const SpanOptions& options, const std::function<Result<void>(ITelemetrySpan&)>& callback) = 0;
    virtual void addEvent(const std::string& name, const Json& attributes = Json::object()) = 0;
    /** Merges `attributes` into the span's. */
    virtual void setAttributes(const Json& attributes) = 0;
    virtual void setStatus(const SpanStatus& status) = 0;
};
