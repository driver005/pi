module;

#include <cstdint>

export module pi.types.conversation_create_options;

import std;
export import pi.support.transaction;
export import pi.types.json;
export import pi.types.result;

/** How a conversation is created or forked. */
export struct ConversationCreateOptions {
    /** `{kind: "ownerless"}` (default) or `{kind: "task", taskId}`: always selected explicitly. */
    Json ownership = Json::object({{"kind", "ownerless"}});
    /** Applied in the creating commit after the creation hook's copy, before `init`: an agent change. */
    std::optional<Json> agent;
    /**
     * Runs inside the creating commit, after the creation hook and the `agent` change. The conversation creation is
     * already a table write, so table reads here fail with `read_after_write`; document access remains available.
     */
    std::function<Result<void>(Transaction&, std::int64_t conversationId)> init;
};
