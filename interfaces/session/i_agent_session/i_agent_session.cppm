module;

#include <cstdint>

export module pi.session.i_agent_session;

import std;
export import pi.types.cache_warming_status;
export import pi.types.agent_message;
export import pi.types.agent_session_event;
export import pi.types.bash_result;
export import pi.types.compaction_result;
export import pi.types.context_usage;
export import pi.types.forkable_message;
export import pi.types.json;
export import pi.types.model;
export import pi.types.model_cycle_result;
export import pi.types.navigate_tree_options;
export import pi.types.navigate_tree_result;
export import pi.types.prompt_disposition;
export import pi.types.prompt_options;
export import pi.types.queue_mode;
export import pi.types.queued_input;
export import pi.types.result;
export import pi.types.scoped_model;
export import pi.types.send_message_options;
export import pi.types.session_entry;
export import pi.types.session_stats;
export import pi.types.session_tree_node;
export import pi.types.slash_command_info;
export import pi.types.thinking_level;
export import pi.types.tool_info;

/**
 * One conversation with an agent: the agent, its persisted session tree, model and thinking
 * selection, compaction, retry, bash commands and tool set. Every client (RPC, server, plugins)
 * drives the agent through this interface. prompt() and the operations that run the model block
 * until the work has settled, so call them from a worker thread; abort() and the other control
 * calls are safe to make from any thread. Port of AgentSession (core/agent-session.ts).
 */
export class IAgentSession {
public:
    using Listener = std::function<void(const AgentSessionEvent&)>;
    using ListenerId = std::uint64_t;

    virtual ~IAgentSession() = default;

    // Events
    virtual ListenerId subscribe(Listener listener) = 0;
    virtual void unsubscribe(ListenerId id) = 0;

    // State
    virtual std::vector<AgentMessage> messages() const = 0;
    virtual Model model() const = 0;
    virtual ThinkingLevel thinkingLevel() const = 0;
    virtual std::string systemPrompt() const = 0;
    virtual std::string sessionId() const = 0;
    virtual std::optional<std::string> sessionFile() const = 0;
    virtual std::optional<std::string> sessionName() const = 0;
    /** A run (including its retries and compaction continuations) is active. */
    virtual bool isStreaming() const = 0;
    /** No run, compaction or branch summary is active. */
    virtual bool isIdle() const = 0;
    virtual bool isCompacting() const = 0;
    virtual bool isRetrying() const = 0;
    virtual int retryAttempt() const = 0;
    virtual QueueMode steeringMode() const = 0;
    virtual QueueMode followUpMode() const = 0;
    virtual std::size_t pendingMessageCount() const = 0;
    virtual std::optional<std::string> lastAssistantText() const = 0;
    virtual SessionStats stats() const = 0;
    virtual std::optional<ContextUsage> contextUsage() const = 0;

    // Prompting
    virtual Result<PromptDisposition> prompt(const std::string& text, const PromptOptions& options) = 0;
    virtual Result<PromptDisposition> steer(const std::string& text, const std::vector<ImageContent>& images) = 0;
    virtual Result<PromptDisposition> followUp(const std::string& text, const std::vector<ImageContent>& images) = 0;
    /** content is a string or an array of content blocks. */
    virtual Result<void> sendCustomMessage(const std::string& customType, const Json& content, bool display,
                                           const Json& details, const SendMessageOptions& options) = 0;
    virtual QueuedInput clearQueue() = 0;
    virtual QueuedInput queued() const = 0;
    virtual void abort() = 0;
    virtual void waitForIdle() = 0;

    // Model and thinking
    virtual Result<void> setModel(const Model& model, bool persist) = 0;
    virtual std::optional<ModelCycleResult> cycleModel(bool forward, bool persist) = 0;
    virtual void setThinkingLevel(ThinkingLevel level, bool persist) = 0;
    virtual std::optional<ThinkingLevel> cycleThinkingLevel(bool persist) = 0;
    virtual std::vector<ThinkingLevel> availableThinkingLevels() const = 0;
    virtual bool supportsThinking() const = 0;
    virtual void setScopedModels(std::vector<ScopedModel> scoped) = 0;
    virtual void setSteeringMode(QueueMode mode) = 0;
    virtual void setFollowUpMode(QueueMode mode) = 0;

    // Compaction and retry
    virtual Result<CompactionResult> compact(const std::optional<std::string>& customInstructions) = 0;
    virtual void abortCompaction() = 0;
    virtual void abortBranchSummary() = 0;
    virtual void setAutoCompactionEnabled(bool enabled) = 0;
    virtual bool autoCompactionEnabled() const = 0;
    virtual void setAutoRetryEnabled(bool enabled) = 0;
    virtual bool autoRetryEnabled() const = 0;
    virtual void abortRetry() = 0;

    // User bash
    virtual Result<BashResult> executeBash(const std::string& command,
                                           const std::function<void(const std::string&)>& onChunk,
                                           bool excludeFromContext, const std::optional<std::string>& id) = 0;
    virtual void recordBashResult(const std::string& command, const BashResult& result, bool excludeFromContext) = 0;
    virtual void abortBash() = 0;
    virtual bool isBashRunning() const = 0;

    // Tools
    virtual std::vector<std::string> activeToolNames() const = 0;
    virtual std::vector<ToolInfo> allTools() const = 0;
    virtual void setActiveToolsByName(const std::vector<std::string>& names) = 0;

    /** What the prompt-cache warmer is doing (inactive while it is off or the session has no environment to read). */
    virtual CacheWarmingStatus cacheWarmingStatus() const = 0;
    /** Persists the `cacheWarming` setting ("off", "streaming" or "idle") and applies it to a running warmer. */
    virtual Result<void> setCacheWarmingMode(const std::string& mode) = 0;

    /**
     * Files a bug report about this session: `{hint?, includeSession?, includeSummary?, delivery: "upload" | "zip", outputPath?}`
     * answers `{id, delivery, path?}`. Uploads go to the Radius gateway; the report holds environment and configuration
     * metadata without secrets, failed-turn diagnostics, and optionally the transcript or a summary the session model writes.
     */
    virtual Result<Json> reportBug(const Json& options) = 0;

    /**
     * Writes the session as a self-contained HTML page (HtmlExporter) and returns the path: `outputPath` (`~` and relative paths
     * resolve against the session's cwd), else `pi-session-<session file name>.html` in the cwd. `theme` is `dark` (also for "")
     * or `light`. Errors: an in-memory session, a session with nothing written yet, an unknown theme, and hosts without export assets.
     */
    virtual Result<std::string> exportHtml(const std::optional<std::string>& outputPath, const std::string& theme) = 0;

    /**
     * Shares the session (SessionSharer): `{theme?}` answers `{via: "radius" | "gist", url, gistUrl?}`. A signed-in Radius account
     * gets an artifact upload of the current branch (with a trailing `pi.share` entry holding the system prompt and the active
     * tool schemas); otherwise the session is exported as HTML to a private gist through the GitHub CLI.
     */
    virtual Result<Json> shareSession(const Json& options) = 0;

    /** Prompt templates and skills a prompt can invoke by name. */
    virtual std::vector<SlashCommandInfo> slashCommands() const = 0;

    // Session tree
    virtual std::vector<SessionEntry> entries() const = 0;
    virtual std::optional<std::string> leafId() const = 0;
    virtual std::vector<SessionTreeNode> tree() const = 0;
    virtual void setSessionName(const std::string& name) = 0;
    virtual Result<NavigateTreeResult> navigateTree(const std::string& targetId,
                                                    const NavigateTreeOptions& options) = 0;
    virtual std::vector<ForkableMessage> forkableMessages() const = 0;

    /** Re-reads settings and resources and rebuilds the system prompt. */
    virtual Result<void> reload() = 0;
    /** Stops everything and drops subscribers; the session must not be used afterwards. */
    virtual void dispose() = 0;
};
