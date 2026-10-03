module;

#include <cstdint>

export module pi.types.transaction_scope;

import std;

/** Defaults a commit binds to: the conversation of `createTask` and the task credited with appended entries. */
export struct TransactionScope {
    std::optional<std::int64_t> conversationId;
    std::optional<std::int64_t> taskId;
};
