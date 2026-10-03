module;

#include <cstdint>

export module pi.types.task_options;

import std;
export import pi.types.json;

/** Creation options of a durable task. */
export struct TaskOptions {
    /** `{kind: "conversation"}` or `{kind: "task", taskId}`: the owner, always named. */
    Json ownership;
    /** Defaults to the owner task's conversation or the transaction's bound conversation. */
    std::optional<std::int64_t> conversationId;
    /** Conversation-owned tasks only: excluded from idle waits, conversation aborts and cascades. */
    bool background = false;
};
