module;

#include <cstdint>

export module pi.support.in_memory_telemetry_span;

import std;
export import pi.support.in_memory_telemetry_recorder;
export import pi.telemetry.i_telemetry_span;
import pi.support.noop_telemetry_span;

/**
 * A span of an InMemoryTelemetryRecorder (id 0 is the root: it records nothing and only starts spans without a parent). A span
 * that already settled starts inert children, so late work never changes what was recorded.
 */
export class InMemoryTelemetrySpan : public ITelemetrySpan {
public:
    InMemoryTelemetrySpan(InMemoryTelemetryRecorder& recorder, std::int64_t id)
        : m_recorder(recorder),
          m_id(id) {}

    Result<void> startChild(const SpanOptions& options, const std::function<Result<void>(ITelemetrySpan&)>& callback) override {
        if (m_id != 0 && m_recorder.settled(m_id)) {
            NoopTelemetrySpan inert;
            return callback(inert);
        }
        const std::int64_t id = m_recorder.begin(m_id == 0 ? std::nullopt : std::optional<std::int64_t>(m_id), options);
        InMemoryTelemetrySpan child(m_recorder, id);
        Result<void> result = callback(child);
        m_recorder.settle(id, result ? std::nullopt : std::optional<Error>(result.error()));
        return result;
    }

    void addEvent(const std::string& name, const Json& attributes) override {
        m_recorder.addEvent(m_id, name, attributes);
    }

    void setAttributes(const Json& attributes) override {
        m_recorder.setAttributes(m_id, attributes);
    }

    void setStatus(const SpanStatus& status) override {
        m_recorder.setStatus(m_id, status);
    }

private:
    InMemoryTelemetryRecorder& m_recorder;
    std::int64_t m_id;
};
