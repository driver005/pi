module;

#include <nlohmann/json.hpp>

export module pi.support.session_entry_codec;

import std;
export import pi.types.json;
export import pi.types.result;
export import pi.types.session_entry;
export import pi.types.session_header;

/** Session file lines <-> typed header/entries. The JSON bodies are kept verbatim. */
export class SessionEntryCodec {
public:
    /** Every parseable JSON line of a JSONL document, in order; blank or malformed lines skipped. */
    std::vector<Json> parseLines(const std::string& content) const;

    /** nullopt for a blank or malformed line. */
    std::optional<Json> parseLine(const std::string& line) const;

    SessionHeader headerFromJson(const Json& json) const;
    SessionEntry entryFromJson(const Json& json) const;
    bool isHeader(const Json& json) const;
    /** Sets type/id/parentId/timestamp from the body so the index fields match it. */
    SessionEntry entryFromBody(Json body) const;

    /** One JSONL line (compact JSON plus newline). */
    std::string line(const Json& json) const;
};

std::optional<Json> SessionEntryCodec::parseLine(const std::string& line) const {
    if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
        return std::nullopt;
    }
    Json json = Json::parse(line, nullptr, false);
    if (json.is_discarded()) {
        return std::nullopt;
    }
    return json;
}

std::vector<Json> SessionEntryCodec::parseLines(const std::string& content) const {
    std::vector<Json> out;
    std::size_t start = 0;
    while (start <= content.size()) {
        const std::size_t end = content.find('\n', start);
        const std::string text =
            content.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (auto json = parseLine(text)) {
            out.push_back(std::move(*json));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

bool SessionEntryCodec::isHeader(const Json& json) const {
    return json.is_object() && json.value("type", "") == "session";
}

SessionHeader SessionEntryCodec::headerFromJson(const Json& json) const {
    SessionHeader header;
    header.body = json;
    header.version = json.contains("version") && json["version"].is_number() ? json["version"].get<int>() : 1;
    header.id = json.value("id", "");
    header.timestamp = json.value("timestamp", "");
    header.cwd = json.contains("cwd") && json["cwd"].is_string() ? json["cwd"].get<std::string>() : "";
    if (json.contains("parentSession") && json["parentSession"].is_string()) {
        header.parentSession = json["parentSession"].get<std::string>();
    }
    return header;
}

SessionEntry SessionEntryCodec::entryFromJson(const Json& json) const {
    SessionEntry entry;
    entry.body = json;
    entry.type = json.value("type", "");
    entry.id = json.contains("id") && json["id"].is_string() ? json["id"].get<std::string>() : "";
    if (json.contains("parentId") && json["parentId"].is_string()) {
        entry.parentId = json["parentId"].get<std::string>();
    }
    entry.timestamp = json.contains("timestamp") && json["timestamp"].is_string()
                          ? json["timestamp"].get<std::string>()
                          : "";
    return entry;
}

SessionEntry SessionEntryCodec::entryFromBody(Json body) const {
    return entryFromJson(body);
}

std::string SessionEntryCodec::line(const Json& json) const {
    return json.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
}
