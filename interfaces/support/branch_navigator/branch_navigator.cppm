export module pi.support.branch_navigator;

import std;
export import pi.agent.i_agent;
export import pi.session.i_session_event_sink;
export import pi.session.i_session_manager;
export import pi.session.i_settings_manager;
export import pi.support.branch_summarizer;
export import pi.support.session_context_refresher;
export import pi.support.summarization_retry_reporter;
export import pi.types.agent_loop_config;
export import pi.types.forkable_message;
export import pi.types.navigate_tree_options;
export import pi.types.navigate_tree_result;

/**
 * Moves the session's current position to another entry of the tree, optionally summarizing the
 * branch that is left behind so its context survives. Port of AgentSession.navigateTree.
 */
export class BranchNavigator {
public:
    BranchNavigator(IAgent& agent, ISessionManager& session, ISettingsManager& settings,
                    SessionContextRefresher& refresher, ISessionEventSink& sink,
                    const BranchSummarizer& summarizer, StreamFn streamFn);

    /** The caller has checked that no run or compaction is active. */
    Result<NavigateTreeResult> navigate(const std::string& targetId, const NavigateTreeOptions& options);

    std::vector<ForkableMessage> forkableMessages() const;

    void abort();
    bool summarizing() const;

private:
    std::string userText(const Json& content) const;
    BranchSummaryOptions summaryOptions(const NavigateTreeOptions& options,
                                        const std::shared_ptr<AbortSignal>& signal) const;
    Result<NavigateTreeResult> moveLeaf(const SessionEntry& target, const std::optional<std::string>& summary,
                                        const Json& details, const std::optional<Usage>& usage,
                                        const std::optional<std::string>& label);
    std::optional<std::string> navigationLeaf(const SessionEntry& target, std::optional<std::string>& editorText) const;

    IAgent& m_agent;
    ISessionManager& m_session;
    ISettingsManager& m_settings;
    SessionContextRefresher& m_refresher;
    const BranchSummarizer& m_summarizer;
    StreamFn m_streamFn;
    SummarizationRetryReporter m_reporter;

    mutable std::mutex m_mutex;
    std::shared_ptr<AbortSignal> m_signal;
};

BranchNavigator::BranchNavigator(IAgent& agent, ISessionManager& session, ISettingsManager& settings,
                                 SessionContextRefresher& refresher, ISessionEventSink& sink,
                                 const BranchSummarizer& summarizer, StreamFn streamFn)
    : m_agent(agent),
      m_session(session),
      m_settings(settings),
      m_refresher(refresher),
      m_summarizer(summarizer),
      m_streamFn(std::move(streamFn)),
      m_reporter(sink) {}

void BranchNavigator::abort() {
    std::shared_ptr<AbortSignal> signal;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        signal = m_signal;
    }
    if (signal) {
        signal->abort();
    }
}

bool BranchNavigator::summarizing() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_signal != nullptr;
}

/** Joined text blocks of a stored user content (a string or an array of blocks). */
std::string BranchNavigator::userText(const Json& content) const {
    if (content.is_string()) {
        return content.get<std::string>();
    }
    std::string text;
    if (content.is_array()) {
        for (const auto& block : content) {
            if (block.value("type", "") == "text" && block.contains("text") && block["text"].is_string()) {
                text += block["text"].get<std::string>();
            }
        }
    }
    return text;
}

std::vector<ForkableMessage> BranchNavigator::forkableMessages() const {
    std::vector<ForkableMessage> out;
    for (const auto& entry : m_session.entries()) {
        if (entry.type != "message" || !entry.body.contains("message")) {
            continue;
        }
        const Json& message = entry.body["message"];
        if (message.value("role", "") != "user" || !message.contains("content")) {
            continue;
        }
        if (const std::string text = userText(message["content"]); !text.empty()) {
            out.push_back(ForkableMessage{entry.id, text});
        }
    }
    return out;
}

BranchSummaryOptions BranchNavigator::summaryOptions(const NavigateTreeOptions& options,
                                                     const std::shared_ptr<AbortSignal>& signal) const {
    BranchSummaryOptions out;
    out.summarization.model = m_agent.model();
    out.summarization.stream.signal = signal;
    out.summarization.thinkingLevel = m_agent.thinkingLevel();
    out.summarization.streamFn = m_streamFn;
    out.summarization.retry = m_settings.view().retryPolicy();
    out.summarization.callbacks = m_reporter.callbacks("branchSummary", "");
    out.customInstructions = options.customInstructions;
    out.replaceInstructions = options.replaceInstructions;
    out.reserveTokens = m_settings.view().branchSummaryReserveTokens();
    return out;
}

/** User and custom messages return to their parent so the client can edit and resubmit them. */
std::optional<std::string> BranchNavigator::navigationLeaf(const SessionEntry& target,
                                                           std::optional<std::string>& editorText) const {
    const bool userMessage = target.type == "message" && target.body.contains("message") &&
                             target.body["message"].value("role", "") == "user";
    if (userMessage) {
        editorText = userText(target.body["message"].value("content", Json("")));
        return target.parentId;
    }
    if (target.type == "custom_message") {
        editorText = userText(target.body.value("content", Json("")));
        return target.parentId;
    }
    return target.id;
}

Result<NavigateTreeResult> BranchNavigator::moveLeaf(const SessionEntry& target,
                                                     const std::optional<std::string>& summary,
                                                     const Json& details, const std::optional<Usage>& usage,
                                                     const std::optional<std::string>& label) {
    NavigateTreeResult result;
    const std::optional<std::string> newLeaf = navigationLeaf(target, result.editorText);
    if (summary) {
        const auto summaryId = m_session.branchWithSummary(newLeaf, *summary, details, false, usage);
        if (!summaryId) {
            return std::unexpected(summaryId.error());
        }
        result.summaryEntry = m_session.entry(*summaryId);
        if (label) {
            if (const auto labelled = m_session.appendLabelChange(*summaryId, label); !labelled) {
                return std::unexpected(labelled.error());
            }
        }
    } else if (!newLeaf) {
        m_session.resetLeaf();
    } else if (const auto branched = m_session.branch(*newLeaf); !branched) {
        return std::unexpected(branched.error());
    }
    if (label && !summary) {
        if (const auto labelled = m_session.appendLabelChange(target.id, label); !labelled) {
            return std::unexpected(labelled.error());
        }
    }
    m_refresher.refresh();
    return result;
}

Result<NavigateTreeResult> BranchNavigator::navigate(const std::string& targetId,
                                                     const NavigateTreeOptions& options) {
    const std::optional<std::string> oldLeaf = m_session.leafId();
    if (targetId == oldLeaf) {
        return NavigateTreeResult{};
    }
    if (options.summarize && m_agent.model().id.empty()) {
        return std::unexpected(Error{"no_model", "No model available for summarization"});
    }
    const auto target = m_session.entry(targetId);
    if (!target) {
        return std::unexpected(Error{"not_found", "Entry " + targetId + " not found"});
    }
    const CollectEntriesResult collected = m_summarizer.collectEntries(m_session, oldLeaf, targetId);
    const auto signal = std::make_shared<AbortSignal>();
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_signal = signal;
    }
    std::optional<std::string> summary;
    Json details;
    std::optional<Usage> usage;
    if (options.summarize && !collected.entries.empty()) {
        const BranchSummaryResult generated =
            m_summarizer.summarize(collected.entries, summaryOptions(options, signal));
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_signal.reset();
        if (generated.aborted) {
            NavigateTreeResult aborted;
            aborted.cancelled = true;
            aborted.aborted = true;
            return aborted;
        }
        if (generated.error) {
            return std::unexpected(Error{"summarization_failed", *generated.error});
        }
        summary = generated.summary;
        usage = generated.usage;
        details = Json{{"readFiles", generated.readFiles.value_or(std::vector<std::string>{})},
                       {"modifiedFiles", generated.modifiedFiles.value_or(std::vector<std::string>{})}};
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_signal.reset();
    }
    return moveLeaf(*target, summary && !summary->empty() ? summary : std::nullopt, details, usage, options.label);
}
