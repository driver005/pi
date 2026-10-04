module;

#include <cstdint>

export module pi.types.recorded_telemetry_span;

import std;
export import pi.types.json;
export import pi.types.recorded_telemetry_event;
export import pi.types.span_status;

/** A snapshot of a span a recording telemetry context saw; endSequence orders the spans that settled. */
export struct RecordedTelemetrySpan {
    std::int64_t id = 0;
    std::optional<std::int64_t> parentId;
    std::string name;
    Json attributes = Json::object();
    std::vector<RecordedTelemetryEvent> events;
    SpanStatus status;
    bool settled = false;
    std::optional<std::int64_t> endSequence;
};
