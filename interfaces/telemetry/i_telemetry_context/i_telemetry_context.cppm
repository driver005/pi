export module pi.telemetry.i_telemetry_context;

import std;
export import pi.telemetry.i_telemetry_span;

/**
 * Where spans start. Applications pass a context explicitly (there is no global current span); the no-op context is what
 * packages use when none is given. Port of TelemetryContext in packages/telemetry.
 */
export class ITelemetryContext {
public:
    virtual ~ITelemetryContext() = default;

    /**
     * Runs `callback` exactly once, synchronously, inside a new span and returns what it returns. The span settles when the
     * callback returns: with an ok status, or, when the callback returned an error and did not set a status itself, with an
     * error status naming that error. The span's failures never reach the caller: only the callback's own result does.
     */
    virtual Result<void> startSpan(const SpanOptions& options, const std::function<Result<void>(ITelemetrySpan&)>& callback) = 0;
};
