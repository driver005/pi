export module pi.support.noop_telemetry_span;

import std;
export import pi.telemetry.i_telemetry_span;

/** A span that records nothing; children run in the same inert span. */
export class NoopTelemetrySpan : public ITelemetrySpan {
public:
    Result<void> startChild(const SpanOptions&, const std::function<Result<void>(ITelemetrySpan&)>& callback) override {
        return callback(*this);
    }

    void addEvent(const std::string&, const Json&) override {}
    void setAttributes(const Json&) override {}
    void setStatus(const SpanStatus&) override {}
};
