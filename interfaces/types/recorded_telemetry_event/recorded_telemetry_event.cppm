export module pi.types.recorded_telemetry_event;

import std;
export import pi.types.json;

/** An event a recording telemetry context saw on a span. */
export struct RecordedTelemetryEvent {
    std::string name;
    Json attributes = Json::object();
};
