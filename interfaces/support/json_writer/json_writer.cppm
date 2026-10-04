export module pi.support.json_writer;

import std;
export import pi.types.json;

/**
 * JSON text output that never fails: invalid UTF-8 (lone bytes from a truncated stream or a
 * binary tool result) becomes U+FFFD instead of aborting serialization.
 */
export class JsonWriter {
public:
    std::string compact(const Json& value) const {
        return value.dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    std::string pretty(const Json& value, int indent = 2) const {
        return value.dump(indent, ' ', false, Json::error_handler_t::replace);
    }
};
