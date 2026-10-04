export module pi.support.entry_kinds;

import std;
export import pi.types.json;

/** The built-in entry kinds of the durable harness. Port of packages/durable/src/entries.ts. */
export class EntryKinds {
public:
    /** User input: `model` is `[UserMessage]`. Written by submissions. */
    std::string user() const {
        return "pi.user";
    }

    /** Provider result with any stop reason: `model` is `[AssistantMessage]`. Written by generation. */
    std::string assistant() const {
        return "pi.assistant";
    }

    /** Positional prompt and tool change: `model` is `[SystemMessage]` with empty content. */
    std::string system() const {
        return "pi.system";
    }

    /** Tool result: `model` is `[ToolResultMessage]`; `data` holds the structured diagnostics. */
    std::string toolResult() const {
        return "pi.tool-result";
    }

    /** Start of a new context: always `head: "self"`, with `model` absent or `[UserMessage]` carrying the handoff. */
    std::string reset() const {
        return "pi.reset";
    }

    /** Compaction summary: `model` is `[UserMessage]` with the wrapped summary, `head` the first kept entry. */
    std::string compaction() const {
        return "pi.compaction";
    }

    /** Whether the entry has this kind. */
    bool is(const std::optional<Json>& entry, const std::string& kind) const {
        return entry && entry->value("kind", std::string()) == kind;
    }
};
