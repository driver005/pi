export module pi.types.span_options;

import std;
export import pi.types.json;

/** What a span starts with: its name and optional attributes (a JSON object of strings, numbers, booleans and arrays of them; null values are dropped). */
export struct SpanOptions {
    std::string name;
    Json attributes = Json::object();
};
