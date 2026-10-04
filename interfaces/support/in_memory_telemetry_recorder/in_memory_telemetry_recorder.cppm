module;

#include <cstdint>

export module pi.support.in_memory_telemetry_recorder;

import std;
export import pi.types.error;
export import pi.types.recorded_telemetry_span;
export import pi.types.span_options;

/**
 * The span records of an InMemoryTelemetryContext. Thread-safe; span ids start at 1 and count in start order, end sequences
 * count in settlement order. Attribute objects are copied without their null values, as the TypeScript reference does.
 */
export class InMemoryTelemetryRecorder {
public:
    /** Records a new span under `parentId` and returns its id. */
    std::int64_t begin(std::optional<std::int64_t> parentId, const SpanOptions& options) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        RecordedTelemetrySpan span;
        span.id = m_nextId++;
        span.parentId = parentId;
        span.name = options.name;
        span.attributes = copied(options.attributes);
        m_spans.push_back(std::move(span));
        return m_spans.back().id;
    }

    bool settled(std::int64_t id) const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const RecordedTelemetrySpan* span = find(id);
        return span != nullptr && span->settled;
    }

    void addEvent(std::int64_t id, const std::string& name, const Json& attributes) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        RecordedTelemetrySpan* span = find(id);
        if (span != nullptr && !span->settled) {
            span->events.push_back(RecordedTelemetryEvent{name, copied(attributes)});
        }
    }

    void setAttributes(std::int64_t id, const Json& attributes) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        RecordedTelemetrySpan* span = find(id);
        if (span == nullptr || span->settled || !attributes.is_object()) {
            return;
        }
        for (const auto& entry : attributes.items()) {
            if (!entry.value().is_null()) {
                span->attributes[entry.key()] = entry.value();
            }
        }
    }

    /** An explicit status; a later automatic error from settle() does not replace it. */
    void setStatus(std::int64_t id, const SpanStatus& status) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        RecordedTelemetrySpan* span = find(id);
        if (span != nullptr && !span->settled) {
            span->status = status;
            m_explicit.insert(id);
        }
    }

    /** Ends the span: `failure` (the callback's error) becomes its status unless one was set explicitly. */
    void settle(std::int64_t id, const std::optional<Error>& failure) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        RecordedTelemetrySpan* span = find(id);
        if (span == nullptr || span->settled) {
            return;
        }
        if (failure && !m_explicit.contains(id)) {
            span->status = SpanStatus{false, failure->code, failure->message};
        }
        span->settled = true;
        span->endSequence = m_nextEnd++;
    }

    /** Snapshots in start order. */
    std::vector<RecordedTelemetrySpan> spans() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_spans;
    }

private:
    Json copied(const Json& attributes) const {
        Json out = Json::object();
        if (attributes.is_object()) {
            for (const auto& entry : attributes.items()) {
                if (!entry.value().is_null()) {
                    out[entry.key()] = entry.value();
                }
            }
        }
        return out;
    }

    RecordedTelemetrySpan* find(std::int64_t id) {
        return id >= 1 && id <= static_cast<std::int64_t>(m_spans.size()) ? &m_spans[static_cast<std::size_t>(id - 1)] : nullptr;
    }

    const RecordedTelemetrySpan* find(std::int64_t id) const {
        return id >= 1 && id <= static_cast<std::int64_t>(m_spans.size()) ? &m_spans[static_cast<std::size_t>(id - 1)] : nullptr;
    }

    mutable std::mutex m_mutex;
    std::vector<RecordedTelemetrySpan> m_spans;
    std::set<std::int64_t> m_explicit;
    std::int64_t m_nextId = 1;
    std::int64_t m_nextEnd = 1;
};
