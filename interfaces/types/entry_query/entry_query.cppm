export module pi.types.entry_query;

import std;

/** Inclusive id bounds for a newest-first scan of one conversation's fork-aware history. */
export struct EntryQuery {
    std::int64_t conversationId = 0;
    std::optional<std::int64_t> minEntryId;
    std::optional<std::int64_t> maxEntryId;
};
