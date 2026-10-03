export module pi.types.task_query;

import std;

/** Optional filters of a task scan; every supplied filter must match. */
export struct TaskQuery {
    std::optional<std::int64_t> conversationId;
    std::optional<std::string> kind;
    /** pending, running, waiting, completing or terminal. */
    std::optional<std::string> status;
    std::optional<bool> abortRequested;
    std::optional<bool> background;
};
