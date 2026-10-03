export module pi.types.document_address;

import std;
export import pi.types.json;

/** Exact logical identity of a singleton (no key) or one keyed family member. */
export struct DocumentAddress {
    std::string kind;
    /** `{kind: "session"}`, `{kind: "conversation", conversationId}` or `{kind: "task", taskId}`. */
    Json scope;
    std::optional<std::string> key;
};
