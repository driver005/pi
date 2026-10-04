export module pi.types.conversation_query;

import std;

/** Optional owner filters of a conversation scan. */
export struct ConversationQuery {
    std::optional<std::int64_t> ownerConversationId;
    std::optional<std::int64_t> ownerTaskId;
};
