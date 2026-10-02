module;

#include <cstdint>

export module pi.session.compaction_controller;

import std;
export import pi.agent.i_agent;
export import pi.session.i_session_event_sink;
export import pi.session.i_session_manager;
export import pi.session.i_settings_manager;
export import pi.support.agent_token_estimator;
export import pi.support.auth_guidance;
export import pi.support.compaction_preparer;
export import pi.support.compactor;
export import pi.support.session_context_refresher;
export import pi.support.summarization_retry_reporter;
export import pi.types.agent_loop_config;
export import pi.types.compaction_result;
export import pi.types.summarization_options;

/**
 * Runs compaction for a session: manual (on request) and automatic (threshold or overflow). Each
 * run prepares what to summarize, asks the model for the summary, appends the compaction entry,
 * reloads the agent's context and reports start/end events. Port of the compaction section of
 * AgentSession.
 */
export class CompactionController {
public:
    CompactionController(IAgent& agent, ISessionManager& session, ISettingsManager& settings,
                         SessionContextRefresher& refresher, ISessionEventSink& sink,
                         const Compactor& compactor, StreamFn streamFn);

    /** The caller has aborted any active run. Error carries "Compaction failed: ..." text. */
    Result<CompactionResult> compactManual(const std::optional<std::string>& customInstructions);

    /**
     * Threshold or overflow compaction. Returns whether the post-run loop should continue the
     * agent: after an overflow retry, or when messages are queued.
     */
    bool runAuto(const std::string& reason, bool willRetry);

    void abort();
    bool compacting() const;

private:
    SummarizationOptions optionsFor(const std::shared_ptr<AbortSignal>& signal, const std::string& reason) const;
    Result<CompactionResult> perform(const CompactionPreparation& preparation,
                                     const std::optional<std::string>& customInstructions,
                                     const std::shared_ptr<AbortSignal>& signal, const std::string& reason);
    std::optional<CompactionPreparation> prepare() const;
    std::string noPreparationReason() const;
    void emitStart(const std::string& reason);
    void emitEnd(const std::string& reason, const std::optional<CompactionResult>& result, bool aborted,
                 bool willRetry, const std::optional<std::string>& errorMessage);
    std::shared_ptr<AbortSignal> begin(std::shared_ptr<AbortSignal>& slot);
    void finish(std::shared_ptr<AbortSignal>& slot, const std::shared_ptr<AbortSignal>& mine);

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

    mutable std::mutex m_mutex;
    std::shared_ptr<AbortSignal> m_manual;
    std::shared_ptr<AbortSignal> m_auto;
};

CompactionController::CompactionController(IAgent& agent, ISessionManager& session,
                                           ISettingsManager& settings, SessionContextRefresher& refresher,
                                           ISessionEventSink& sink, const Compactor& compactor,
                                           StreamFn streamFn)
    : m_agent(agent),
      m_session(session),
      m_settings(settings),
      m_refresher(refresher),
      m_sink(sink),
      m_compactor(compactor),
      m_streamFn(std::move(streamFn)),
      m_reporter(sink) {}

std::shared_ptr<AbortSignal> CompactionController::begin(std::shared_ptr<AbortSignal>& slot) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    slot = std::make_shared<AbortSignal>();
    return slot;
}

void CompactionController::finish(std::shared_ptr<AbortSignal>& slot,
                                  const std::shared_ptr<AbortSignal>& mine) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (slot == mine) {
        slot.reset();
    }
}

void CompactionController::abort() {
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

bool CompactionController::compacting() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_manual != nullptr || m_auto != nullptr;
}

SummarizationOptions CompactionController::optionsFor(const std::shared_ptr<AbortSignal>& signal,
                                                      const std::string& reason) const {
    SummarizationOptions options;
    options.model = m_agent.model();
    options.stream.signal = signal;
    options.thinkingLevel = m_agent.thinkingLevel();
    options.streamFn = m_streamFn;
    options.retry = m_settings.view().retryPolicy();
    options.callbacks = m_reporter.callbacks("compaction", reason);
    return options;
}

std::optional<CompactionPreparation> CompactionController::prepare() const {
    const Model model = m_agent.model();
    const CompactionSettings settings = m_settings.view().compactionSettings(model.provider, model.id);
    return m_preparer.prepare(m_session.branchPath(), settings);
}

std::string CompactionController::noPreparationReason() const {
    const auto path = m_session.branchPath();
    return !path.empty() && path.back().type == "compaction" ? "Already compacted"
                                                              : "Nothing to compact (session too small)";
}

void CompactionController::emitStart(const std::string& reason) {
    AgentSessionEvent event;
    event.type = SessionEventType::CompactionStart;
    event.reason = reason;
    m_sink.emit(event);
}

void CompactionController::emitEnd(const std::string& reason, const std::optional<CompactionResult>& result,
                                   bool aborted, bool willRetry, const std::optional<std::string>& errorMessage) {
    AgentSessionEvent event;
    event.type = SessionEventType::CompactionEnd;
    event.reason = reason;
    event.result = result;
    event.aborted = aborted;
    event.willRetry = willRetry;
    event.errorMessage = errorMessage;
    m_sink.emit(event);
}

Result<CompactionResult> CompactionController::perform(const CompactionPreparation& preparation,
                                                       const std::optional<std::string>& customInstructions,
                                                       const std::shared_ptr<AbortSignal>& signal,
                                                       const std::string& reason) {
    auto compacted = m_compactor.compact(preparation, customInstructions, optionsFor(signal, reason));
    if (!compacted) {
        const bool aborted = compacted.error().code == "aborted";
        return std::unexpected(aborted ? Error{"aborted", "Compaction cancelled"} : compacted.error());
    }
    if (signal->aborted()) {
        return std::unexpected(Error{"aborted", "Compaction cancelled"});
    }
    const auto appended = m_session.appendCompaction(compacted->summary, compacted->firstKeptEntryId,
                                                     compacted->tokensBefore, compacted->details, false,
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
    return compacted;
}

Result<CompactionResult> CompactionController::compactManual(const std::optional<std::string>& customInstructions) {
    const auto signal = begin(m_manual);
    emitStart("manual");
    const auto fail = [&](const Error& error) -> Result<CompactionResult> {
        const bool aborted = signal->aborted() || error.code == "aborted";
        finish(m_manual, signal);
        emitEnd("manual", std::nullopt, aborted, false,
                aborted ? std::nullopt : std::optional<std::string>("Compaction failed: " + error.message));
        return std::unexpected(error);
    };
    if (m_agent.model().id.empty()) {
        return fail(Error{"no_model", m_guidance.noModelSelected()});
    }
    const auto preparation = prepare();
    if (!preparation) {
        return fail(Error{"nothing_to_compact", noPreparationReason()});
    }
    auto result = perform(*preparation, customInstructions, signal, "manual");
    if (!result) {
        return fail(result.error());
    }
    finish(m_manual, signal);
    emitEnd("manual", result.value(), false, false, std::nullopt);
    return result;
}

bool CompactionController::runAuto(const std::string& reason, bool willRetry) {
    if (m_agent.model().id.empty()) {
        return false;
    }
    const auto preparation = prepare();
    if (!preparation) {
        return false;
    }
    const auto signal = begin(m_auto);
    emitStart(reason);
    auto result = perform(*preparation, std::nullopt, signal, reason);
    finish(m_auto, signal);
    if (!result) {
        const bool aborted = signal->aborted() || result.error().code == "aborted";
        const std::string prefix =
            reason == "overflow" ? "Context overflow recovery failed: " : "Auto-compaction failed: ";
        emitEnd(reason, std::nullopt, aborted, false,
                aborted ? std::nullopt : std::optional<std::string>(prefix + result.error().message));
        return false;
    }
    emitEnd(reason, result.value(), false, willRetry, std::nullopt);
    return willRetry || m_agent.hasQueuedMessages();
}
