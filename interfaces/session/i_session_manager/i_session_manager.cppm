module;

#include <cstdint>

export module pi.session.i_session_manager;

import std;
export import pi.types.agent_message;
export import pi.types.json;
export import pi.types.result;
export import pi.types.session_context;
export import pi.types.session_entry;
export import pi.types.session_header;
export import pi.types.session_projection;
export import pi.types.session_tree_node;
export import pi.types.usage;

/**
 * A conversation stored as an append-only tree of entries (JSONL file, format version 3). The
 * leaf pointer marks the current position; appending creates a child of the leaf, branching moves
 * the leaf to an earlier entry. Append methods return the new entry's id.
 */
export class ISessionManager {
public:
    virtual ~ISessionManager() = default;

    virtual std::string cwd() const = 0;
    virtual std::string sessionDir() const = 0;
    virtual std::string sessionId() const = 0;
    virtual std::optional<std::string> sessionFile() const = 0;
    virtual bool isPersisted() const = 0;

    /** Starts an empty session; returns the new file path when persisting. */
    virtual Result<std::optional<std::string>> newSession(
        const std::optional<std::string>& id = std::nullopt,
        const std::optional<std::string>& parentSession = std::nullopt) = 0;
    /** Switches to another session file (resume). A missing file starts a new session there. */
    virtual Result<void> setSessionFile(const std::string& path) = 0;

    virtual Result<std::string> appendMessage(const AgentMessage& message) = 0;
    virtual Result<std::string> appendThinkingLevelChange(const std::string& thinkingLevel) = 0;
    virtual Result<std::string> appendModelChange(const std::string& provider,
                                                  const std::string& modelId) = 0;
    virtual Result<SessionEntry> appendUsage(const std::string& kind, const std::string& provider,
                                             const std::string& model, const Usage& usage,
                                             const std::optional<std::string>& note) = 0;
    virtual Result<std::string> appendCompaction(const std::string& summary,
                                                 const std::optional<std::string>& firstKeptEntryId,
                                                 std::int64_t tokensBefore, const Json& details,
                                                 std::optional<bool> fromHook,
                                                 const std::optional<Usage>& usage) = 0;
    virtual Result<std::string> appendCustomEntry(const std::string& customType, const Json& data) = 0;
    virtual Result<std::string> appendSessionInfo(const std::string& name) = 0;
    virtual Result<std::string> appendCustomMessageEntry(const std::string& customType,
                                                         const Json& content, bool display,
                                                         const Json& details) = 0;
    /** replacement nullopt omits the target from context; else {"content": ...}. */
    virtual Result<std::string> appendContextEdit(const std::string& targetId,
                                                  const std::optional<Json>& replacement) = 0;
    virtual Result<std::string> appendLabelChange(const std::string& targetId,
                                                  const std::optional<std::string>& label) = 0;

    virtual std::optional<std::string> leafId() const = 0;
    virtual std::optional<SessionEntry> leafEntry() const = 0;
    virtual std::optional<SessionEntry> entry(const std::string& id) const = 0;
    virtual std::vector<SessionEntry> children(const std::string& parentId) const = 0;
    virtual std::optional<std::string> label(const std::string& id) const = 0;
    /** Root-to-entry path; from the leaf when fromId is nullopt. */
    virtual std::vector<SessionEntry> branchPath(const std::optional<std::string>& fromId = std::nullopt) const = 0;
    virtual std::vector<SessionEntry> buildContextEntries() const = 0;
    virtual SessionProjection buildSessionProjection() const = 0;
    virtual SessionContext buildSessionContext() const = 0;
    virtual std::optional<SessionHeader> header() const = 0;
    virtual std::size_t entryCount() const = 0;
    virtual std::vector<SessionEntry> entries() const = 0;
    virtual std::vector<SessionTreeNode> tree() const = 0;
    virtual std::optional<std::string> sessionName() const = 0;

    virtual Result<void> branch(const std::string& branchFromId) = 0;
    virtual void resetLeaf() = 0;
    virtual Result<std::string> branchWithSummary(const std::optional<std::string>& branchFromId,
                                                  const std::string& summary, const Json& details,
                                                  std::optional<bool> fromHook,
                                                  const std::optional<Usage>& usage) = 0;
    /** New session containing only the path to leafId; returns its file when persisting. */
    virtual Result<std::optional<std::string>> createBranchedSession(const std::string& leafId) = 0;
};
