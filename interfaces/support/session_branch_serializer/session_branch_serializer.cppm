module;

#include <cstdint>

export module pi.support.session_branch_serializer;

import std;
export import pi.platform.i_clock;
export import pi.types.json;
export import pi.types.session_entry;
import pi.support.iso_timestamp;

/**
 * The current branch of a session as a standalone JSONL file: a version 3 session header, the branch entries chained through
 * fresh parent ids (the first entry has none) and optional trailing entries. Port of serializeSessionBranch in
 * core/session-export.ts.
 */
export class SessionBranchSerializer {
public:
    explicit SessionBranchSerializer(const IClock& clock)
        : m_clock(clock) {}

    /** `trailing(parentId, timestamp)` adds entries after the branch (export-only metadata); may be empty. */
    std::string serialize(const std::string& sessionId, const std::string& cwd, const std::vector<SessionEntry>& branch, const std::function<std::vector<Json>(const std::optional<std::string>&, const std::string&)>& trailing = {}) const {
        const std::string timestamp = m_time.format(m_clock.nowMs());
        std::string out = Json::object({{"type", "session"}, {"version", 3}, {"id", sessionId}, {"timestamp", timestamp}, {"cwd", cwd}}).dump() + "\n";
        std::optional<std::string> parent;
        for (const SessionEntry& entry : branch) {
            Json body = entry.body;
            body["parentId"] = parent ? Json(*parent) : Json(nullptr);
            out += body.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
            parent = entry.id;
        }
        if (trailing) {
            for (const Json& entry : trailing(parent, timestamp)) {
                out += entry.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
            }
        }
        return out;
    }

private:
    const IClock& m_clock;
    IsoTimestamp m_time;
};
