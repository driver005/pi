export module pi.support.noop_telemetry_context;

import std;
export import pi.telemetry.i_telemetry_context;
import pi.support.noop_telemetry_span;

/** The context packages use when an application provides none: spans run their callback and record nothing. */
export class NoopTelemetryContext : public ITelemetryContext {
public:
    Result<void> startSpan(const SpanOptions&, const std::function<Result<void>(ITelemetrySpan&)>& callback) override {
        NoopTelemetrySpan span;
        return callback(span);
    }
};
