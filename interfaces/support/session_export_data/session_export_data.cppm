export module pi.support.session_export_data;

import std;
export import pi.session.i_session_manager;
export import pi.types.json;

/**
 * The JSON the HTML export page reads: `{header, entries, leafId, systemPrompt?, tools?}` with the header and entries as they are in
 * the session file. A live session adds its system prompt and active tools (`{name, description, parameters}`); an exported file has none.
 */
export class SessionExportData {
public:
    Json build(const ISessionManager& session, const std::optional<std::string>& systemPrompt, const std::optional<Json>& tools) const {
        Json entries = Json::array();
        for (const SessionEntry& entry : session.entries()) {
            entries.push_back(entry.body);
        }
        const auto header = session.header();
        const auto leaf = session.leafId();
        Json data{{"header", header ? header->body : Json()}, {"entries", entries}, {"leafId", leaf ? Json(*leaf) : Json()}};
        if (systemPrompt) {
            data["systemPrompt"] = *systemPrompt;
        }
        if (tools) {
            data["tools"] = *tools;
        }
        return data;
    }
};
