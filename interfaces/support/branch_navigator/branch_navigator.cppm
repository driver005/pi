export module pi.support.branch_navigator;

import std;
export import pi.agent.i_agent;
export import pi.session.i_session_event_sink;
export import pi.session.i_session_manager;
export import pi.session.i_settings_manager;
export import pi.support.branch_summarizer;
export import pi.support.plugin_session_events;
export import pi.support.session_context_refresher;
export import pi.support.summarization_retry_reporter;
export import pi.types.agent_loop_config;
export import pi.types.forkable_message;
export import pi.types.navigate_tree_options;
export import pi.types.navigate_tree_result;
export import pi.types.routed_selection;

/**
 * Moves the session's current position to another entry of the tree, optionally summarizing the
 * branch that is left behind so its context survives. Port of AgentSession.navigateTree. With plugin
 * events attached, plugins may cancel a navigation, supply the summary or change how it is made
 * (`session_before_tree`) and observe the move (`session_tree`).
 */
export class BranchNavigator {
public:
    BranchNavigator(IAgent& agent, ISessionManager& session, ISettingsManager& settings, SessionContextRefresher& refresher, ISessionEventSink& sink, const BranchSummarizer& summarizer, StreamFn streamFn)
        : m_agent(agent),
          m_session(session),
          m_settings(settings),
          m_refresher(refresher),
          m_summarizer(summarizer),
          m_streamFn(std::move(streamFn)),
          m_reporter(sink) {}

    /** Plugin events to fire; nullptr (the default) fires none. The pointee must outlive the navigator. */
    void setEvents(PluginSessionEvents* events) {
        m_events = events;
    }

    /**
     * The model and thinking level that write the summary: the agent's by default. A host with virtual models routes the
     * request here, so the summary is sized and sent for the physical model; an error fails the navigation.
     */
    using SummaryModel = std::function<Result<RoutedSelection>()>;

    void setSummaryModel(SummaryModel source) {
        m_summaryModel = std::move(source);
    }

    /** The caller has checked that no run or compaction is active. */
    Result<NavigateTreeResult> navigate(const std::string& targetId, const NavigateTreeOptions& requested) {
        NavigateTreeOptions options = requested;
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
        bool fromExtension = false;
        if (m_events != nullptr) {
            BeforeTreeOutcome decision = m_events->beforeTree(preparation(targetId, oldLeaf, collected, options));
            if (decision.cancel) {
                finishSummary();
                NavigateTreeResult cancelled;
                cancelled.cancelled = true;
                return cancelled;
            }
            if (decision.summary && options.summarize) {
                summary = std::move(decision.summary);
                details = std::move(decision.details);
                usage = std::move(decision.usage);
                fromExtension = true;
            }
            if (decision.customInstructions) {
                options.customInstructions = std::move(decision.customInstructions);
            }
            if (decision.replaceInstructions) {
                options.replaceInstructions = *decision.replaceInstructions;
            }
            if (decision.label) {
                options.label = std::move(decision.label);
            }
        }
        if (options.summarize && !summary && !collected.entries.empty()) {
            auto selection = m_summaryModel ? m_summaryModel() : Result<RoutedSelection>(RoutedSelection{m_agent.model(), m_agent.thinkingLevel()});
            if (!selection) {
                finishSummary();
                return std::unexpected(selection.error());
            }
            const BranchSummaryResult generated =
                m_summarizer.summarize(collected.entries, summaryOptions(options, signal, *selection));
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
        finishSummary();
        auto moved = moveLeaf(*target, summary && !summary->empty() ? summary : std::nullopt, details, usage, options.label, fromExtension);
        if (moved && m_events != nullptr) {
            const bool summarized = moved->summaryEntry.has_value();
            m_events->treeNavigated(m_session.leafId(), oldLeaf, moved->summaryEntry, summarized && fromExtension);
        }
        return moved;
    }

    std::vector<ForkableMessage> forkableMessages() const {
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

    void abort() {
        std::shared_ptr<AbortSignal> signal;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            signal = m_signal;
        }
        if (signal) {
            signal->abort();
        }
    }

    bool summarizing() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_signal != nullptr;
    }

private:
    void finishSummary() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_signal.reset();
    }

    TreePreparation preparation(const std::string& targetId, const std::optional<std::string>& oldLeaf, const CollectEntriesResult& collected, const NavigateTreeOptions& options) const {
        TreePreparation out;
        out.targetId = targetId;
        out.oldLeafId = oldLeaf;
        out.commonAncestorId = collected.commonAncestorId;
        out.entriesToSummarize = collected.entries;
        out.userWantsSummary = options.summarize;
        out.customInstructions = options.customInstructions;
        out.replaceInstructions = options.replaceInstructions;
        out.label = options.label;
        return out;
    }

    std::string userText(const Json& content) const {
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

    BranchSummaryOptions summaryOptions(const NavigateTreeOptions& options, const std::shared_ptr<AbortSignal>& signal, const RoutedSelection& selection) const {
        BranchSummaryOptions out;
        out.summarization.model = selection.model;
        out.summarization.stream.signal = signal;
        out.summarization.thinkingLevel = selection.thinkingLevel.value_or(ThinkingLevel::Off);
        out.summarization.streamFn = m_streamFn;
        out.summarization.retry = m_settings.view().retryPolicy();
        out.summarization.callbacks = m_reporter.callbacks("branchSummary", "");
        out.customInstructions = options.customInstructions;
        out.replaceInstructions = options.replaceInstructions;
        out.reserveTokens = m_settings.view().branchSummaryReserveTokens();
        return out;
    }

    Result<NavigateTreeResult> moveLeaf(const SessionEntry& target, const std::optional<std::string>& summary, const Json& details, const std::optional<Usage>& usage, const std::optional<std::string>& label, bool fromHook) {
        NavigateTreeResult result;
        const std::optional<std::string> newLeaf = navigationLeaf(target, result.editorText);
        if (summary) {
            const auto summaryId = m_session.branchWithSummary(newLeaf, *summary, details, fromHook, usage);
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

    std::optional<std::string> navigationLeaf(const SessionEntry& target, std::optional<std::string>& editorText) const {
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

    IAgent& m_agent;
    ISessionManager& m_session;
    ISettingsManager& m_settings;
    SessionContextRefresher& m_refresher;
    const BranchSummarizer& m_summarizer;
    StreamFn m_streamFn;
    SummarizationRetryReporter m_reporter;
    PluginSessionEvents* m_events = nullptr;
    SummaryModel m_summaryModel;

    mutable std::mutex m_mutex;
    std::shared_ptr<AbortSignal> m_signal;
};

/** Joined text blocks of a stored user content (a string or an array of blocks). */

/** User and custom messages return to their parent so the client can edit and resubmit them. */
