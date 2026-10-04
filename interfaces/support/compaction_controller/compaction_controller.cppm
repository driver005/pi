module;

#include <cstdint>

export module pi.support.compaction_controller;

import std;
export import pi.agent.i_agent;
export import pi.session.i_session_event_sink;
export import pi.session.i_session_manager;
export import pi.session.i_settings_manager;
export import pi.support.agent_token_estimator;
export import pi.support.auth_guidance;
export import pi.support.compaction_preparer;
export import pi.support.compactor;
export import pi.support.plugin_session_events;
export import pi.support.session_context_refresher;
export import pi.support.summarization_retry_reporter;
export import pi.types.agent_loop_config;
export import pi.types.compaction_result;
export import pi.types.summarization_options;

/**
 * Runs compaction for a session: manual (on request) and automatic (threshold or overflow). Each
 * run prepares what to summarize, asks the model for the summary, appends the compaction entry,
 * reloads the agent's context and reports start/end events. Port of the compaction section of
 * AgentSession. With plugin events attached, plugins may cancel a compaction or supply its content
 * (`session_before_compact`) and observe the outcome (`session_compact`, `session_compact_failed`).
 */
export class CompactionController {
public:
    CompactionController(IAgent& agent, ISessionManager& session, ISettingsManager& settings, SessionContextRefresher& refresher, ISessionEventSink& sink, const Compactor& compactor, StreamFn streamFn)
        : m_agent(agent),
          m_session(session),
          m_settings(settings),
          m_refresher(refresher),
          m_sink(sink),
          m_compactor(compactor),
          m_streamFn(std::move(streamFn)),
          m_reporter(sink) {}

    /** Plugin events to fire; nullptr (the default) fires none. The pointee must outlive the controller. */
    void setEvents(PluginSessionEvents* events) {
        m_events = events;
    }

    /** The caller has aborted any active run. Error carries "Compaction failed: ..." text. */
    Result<CompactionResult> compactManual(const std::optional<std::string>& customInstructions) {
        const auto signal = begin(m_manual);
        emitStart("manual");
        bool fromExtension = false;
        const auto fail = [&](const Error& error) -> Result<CompactionResult> {
            const bool aborted = signal->aborted() || error.code == "aborted";
            finish(m_manual, signal);
            const std::optional<std::string> message =
                aborted ? std::nullopt : std::optional<std::string>("Compaction failed: " + error.message);
            emitEnd("manual", std::nullopt, aborted, false, message);
            reportFailure("manual", message, aborted, false, fromExtension);
            return std::unexpected(error);
        };
        if (m_agent.model().id.empty()) {
            return fail(Error{"no_model", m_guidance.noModelSelected()});
        }
        const auto preparation = prepare();
        if (!preparation) {
            return fail(Error{"nothing_to_compact", noPreparationReason()});
        }
        auto result = perform(*preparation, customInstructions, signal, "manual", false, fromExtension);
        if (!result) {
            return fail(result.error());
        }
        finish(m_manual, signal);
        emitEnd("manual", result.value(), false, false, std::nullopt);
        return result;
    }

    /**
     * Threshold or overflow compaction. Returns whether the post-run loop should continue the
     * agent: after an overflow retry, or when messages are queued.
     */
    bool runAuto(const std::string& reason, bool willRetry) {
        if (m_agent.model().id.empty()) {
            return false;
        }
        const auto preparation = prepare();
        if (!preparation) {
            return false;
        }
        const auto signal = begin(m_auto);
        emitStart(reason);
        bool fromExtension = false;
        auto result = perform(*preparation, std::nullopt, signal, reason, willRetry, fromExtension);
        finish(m_auto, signal);
        if (!result) {
            const bool aborted = signal->aborted() || result.error().code == "aborted";
            const std::string prefix =
                reason == "overflow" ? "Context overflow recovery failed: " : "Auto-compaction failed: ";
            const std::optional<std::string> message =
                aborted ? std::nullopt : std::optional<std::string>(prefix + result.error().message);
            emitEnd(reason, std::nullopt, aborted, false, message);
            reportFailure(reason, message, aborted, willRetry, fromExtension);
            return false;
        }
        emitEnd(reason, result.value(), false, willRetry, std::nullopt);
        return willRetry || m_agent.hasQueuedMessages();
    }

    void abort() {
        std::shared_ptr<AbortSignal> manual;
        std::shared_ptr<AbortSignal> automatic;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            manual = m_manual;
            automatic = m_auto;
        }
        if (manual) {
            manual->abort();
        }
        if (automatic) {
            automatic->abort();
        }
    }

    bool compacting() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_manual != nullptr || m_auto != nullptr;
    }

    /** A user-requested compaction is running (automatic ones only run inside an agent run). */
    bool manualInProgress() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_manual != nullptr;
    }

private:
    SummarizationOptions optionsFor(const std::shared_ptr<AbortSignal>& signal, const std::string& reason) const {
        SummarizationOptions options;
        options.model = m_agent.model();
        options.stream.signal = signal;
        options.thinkingLevel = m_agent.thinkingLevel();
        options.streamFn = m_streamFn;
        options.retry = m_settings.view().retryPolicy();
        options.callbacks = m_reporter.callbacks("compaction", reason);
        return options;
    }

    Result<CompactionResult> perform(const CompactionPreparation& preparation, const std::optional<std::string>& customInstructions, const std::shared_ptr<AbortSignal>& signal, const std::string& reason, bool willRetry, bool& fromExtension) {
        std::optional<CompactionResult> supplied;
        if (m_events != nullptr) {
            BeforeCompactOutcome decision = m_events->beforeCompact(preparation, m_session.branchPath(), customInstructions, reason, willRetry);
            if (decision.cancel) {
                return std::unexpected(Error{"aborted", "Compaction cancelled"});
            }
            supplied = std::move(decision.compaction);
        }
        fromExtension = supplied.has_value();
        auto compacted = supplied ? Result<CompactionResult>(std::move(*supplied))
                                  : m_compactor.compact(preparation, customInstructions, optionsFor(signal, reason));
        if (!compacted) {
            const bool aborted = compacted.error().code == "aborted";
            return std::unexpected(aborted ? Error{"aborted", "Compaction cancelled"} : compacted.error());
        }
        if (signal->aborted()) {
            return std::unexpected(Error{"aborted", "Compaction cancelled"});
        }
        const auto appended = m_session.appendCompaction(compacted->summary, compacted->firstKeptEntryId,
                                                         compacted->tokensBefore, compacted->details,
                                                         fromExtension ? std::optional<bool>(true) : std::optional<bool>(false),
                                                         compacted->usage);
        if (!appended) {
            return std::unexpected(appended.error());
        }
        m_refresher.refresh();
        std::int64_t after = 0;
        for (const auto& message : m_session.buildSessionProjection().messages) {
            after += m_estimator.messageTokens(message);
        }
        compacted->estimatedTokensAfter = after;
        if (m_events != nullptr) {
            if (const auto saved = m_session.entry(*appended)) {
                m_events->compacted(*saved, fromExtension, reason, willRetry);
            }
        }
        return compacted;
    }

    void reportFailure(const std::string& reason, const std::optional<std::string>& message, bool aborted, bool willRetry, bool fromExtension) {
        if (m_events != nullptr) {
            m_events->compactFailed(reason, message, aborted, willRetry, fromExtension);
        }
    }

    std::optional<CompactionPreparation> prepare() const {
        const Model model = m_agent.model();
        const CompactionSettings settings = m_settings.view().compactionSettings(model.provider, model.id);
        return m_preparer.prepare(m_session.branchPath(), settings);
    }

    std::string noPreparationReason() const {
        const auto path = m_session.branchPath();
        return !path.empty() && path.back().type == "compaction" ? "Already compacted"
                                                                  : "Nothing to compact (session too small)";
    }

    void emitStart(const std::string& reason) {
        AgentSessionEvent event;
        event.type = SessionEventType::CompactionStart;
        event.reason = reason;
        m_sink.emit(event);
    }

    void emitEnd(const std::string& reason, const std::optional<CompactionResult>& result, bool aborted, bool willRetry, const std::optional<std::string>& errorMessage) {
        AgentSessionEvent event;
        event.type = SessionEventType::CompactionEnd;
        event.reason = reason;
        event.result = result;
        event.aborted = aborted;
        event.willRetry = willRetry;
        event.errorMessage = errorMessage;
        m_sink.emit(event);
    }

    std::shared_ptr<AbortSignal> begin(std::shared_ptr<AbortSignal>& slot) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        slot = std::make_shared<AbortSignal>();
        return slot;
    }

    void finish(std::shared_ptr<AbortSignal>& slot, const std::shared_ptr<AbortSignal>& mine) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (slot == mine) {
            slot.reset();
        }
    }

    IAgent& m_agent;
    ISessionManager& m_session;
    ISettingsManager& m_settings;
    SessionContextRefresher& m_refresher;
    ISessionEventSink& m_sink;
    const Compactor& m_compactor;
    StreamFn m_streamFn;
    CompactionPreparer m_preparer;
    AgentTokenEstimator m_estimator;
    SummarizationRetryReporter m_reporter;
    AuthGuidance m_guidance;
    PluginSessionEvents* m_events = nullptr;

    mutable std::mutex m_mutex;
    std::shared_ptr<AbortSignal> m_manual;
    std::shared_ptr<AbortSignal> m_auto;
};
