export module pi.support.typed_span_starter;

import std;
export import pi.telemetry.i_telemetry_context;
import pi.support.span_context;

/**
 * A span starter bound to one explicit parent context: `start(name, attributes, callback)` begins a span and hands the callback
 * a starter bound to that span, so child spans nest. Port of createTypedSpanStarter without the compile-time schema typing;
 * attributes can be checked with TelemetrySchemaValidator.
 */
export class TypedSpanStarter {
public:
    explicit TypedSpanStarter(ITelemetryContext& context)
        : m_context(context) {}

    Result<void> start(const std::string& name, const Json& attributes, const std::function<Result<void>(ITelemetrySpan&, TypedSpanStarter&)>& callback) {
        return m_context.startSpan(SpanOptions{name, attributes}, [&callback](ITelemetrySpan& span) -> Result<void> {
            SpanContext context(span);
            TypedSpanStarter child(context);
            return callback(span, child);
        });
    }

private:
    ITelemetryContext& m_context;
};
