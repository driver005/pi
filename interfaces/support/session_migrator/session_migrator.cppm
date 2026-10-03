export module pi.support.session_migrator;

import std;
export import pi.support.short_id_generator;
export import pi.types.json;

/**
 * Brings session file entries (header first) up to the current version.
 * v1 -> v2 adds the id/parentId tree, v2 -> v3 renames the "hookMessage" role to "custom".
 * Port of migrateToCurrentVersion in core/session-manager.ts.
 */
export class SessionMigrator {
public:
    static constexpr int CurrentVersion = 3;

    /** Mutates the entries in place. True when anything changed. */
    bool migrate(std::vector<Json>& entries);

private:
    void v1ToV2(std::vector<Json>& entries);
    void v2ToV3(std::vector<Json>& entries);

    ShortIdGenerator m_ids;
};

bool SessionMigrator::migrate(std::vector<Json>& entries) {
    int version = 1;
    for (const auto& entry : entries) {
        if (entry.is_object() && entry.value("type", "") == "session") {
            version = entry.contains("version") && entry["version"].is_number() ? entry["version"].get<int>() : 1;
            break;
        }
    }
    if (version >= CurrentVersion) {
        return false;
    }
    if (version < 2) {
        v1ToV2(entries);
    }
    if (version < 3) {
        v2ToV3(entries);
    }
    return true;
}

void SessionMigrator::v1ToV2(std::vector<Json>& entries) {
    std::set<std::string> ids;
    std::optional<std::string> previous;
    // firstKeptEntryIndex counts positions in the file, header included.
    std::vector<std::string> idAtIndex(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Json& entry = entries[i];
        if (!entry.is_object()) {
            continue;
        }
        if (entry.value("type", "") == "session") {
            entry["version"] = 2;
            continue;
        }
        const std::string id = m_ids.next([&](const std::string& candidate) { return ids.contains(candidate); });
        ids.insert(id);
        entry["id"] = id;
        entry["parentId"] = previous ? Json(*previous) : Json(nullptr);
        previous = id;
        idAtIndex[i] = id;
    }
    for (auto& entry : entries) {
        if (!entry.is_object() || entry.value("type", "") != "compaction") {
            continue;
        }
        if (entry.contains("firstKeptEntryIndex") && entry["firstKeptEntryIndex"].is_number()) {
            const auto index = entry["firstKeptEntryIndex"].get<std::size_t>();
            if (index < idAtIndex.size() && !idAtIndex[index].empty()) {
                entry["firstKeptEntryId"] = idAtIndex[index];
            }
            entry.erase("firstKeptEntryIndex");
        }
    }
}

void SessionMigrator::v2ToV3(std::vector<Json>& entries) {
    for (auto& entry : entries) {
        if (!entry.is_object()) {
            continue;
        }
        const std::string type = entry.value("type", "");
        if (type == "session") {
            entry["version"] = 3;
        } else if (type == "message" && entry.contains("message") && entry["message"].is_object() &&
                   entry["message"].value("role", "") == "hookMessage") {
            entry["message"]["role"] = "custom";
        }
    }
}
