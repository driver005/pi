export module pi.types.prompt_operation;

import std;

/** A prompt or compaction a controller started, and how it ended; waited on by operation id. */
export struct PromptOperation {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    /** "done" or "unanswered" once done. */
    std::string status;
    std::optional<std::string> text;
    std::optional<std::string> reason;
};
