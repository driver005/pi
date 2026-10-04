module;

#include <cstdint>

export module pi.types.transaction_task;

import std;
export import pi.types.json;

/** Committed and candidate state of one task a transaction touched. */
export struct TransactionTask {
    std::optional<Json> committed;
    bool committedRead = false;
    /** "create" or "replace" with the candidate record, when written. */
    std::string writeKind;
    Json write;
    std::optional<std::int64_t> publicationConversationId;
};
