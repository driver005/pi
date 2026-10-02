export module pi.types.collect_entries_result;

import std;
export import pi.types.session_entry;

/** Entries abandoned by navigating away (chronological) and the shared ancestor of both ends. */
export struct CollectEntriesResult {
    std::vector<SessionEntry> entries;
    std::optional<std::string> commonAncestorId;
};
