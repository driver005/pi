export module pi.support.post_run_handler;

import std;
export import pi.agent.i_agent;
export import pi.session.i_session_event_sink;
export import pi.session.i_session_manager;
export import pi.session.i_settings_manager;
export import pi.support.agent_token_estimator;
export import pi.support.auto_retry_controller;
export import pi.support.compaction_controller;
export import pi.support.compaction_trigger;
export import pi.support.message_entry_locator;
export import pi.support.recovery_attempt_omitter;
export import pi.types.compaction_check;

/**
 * Decides what happens after an agent run ended: retry a transient failure, compact an
 * overflowing or large context (re-running the turn once after an overflow), or simply deliver
 * queued messages. The result tells the session whether to continue the agent. Port of
 * AgentSession._handlePostAgentRun and _checkCompaction.
 */
export class PostRunHandler {
public:
    PostRunHandler(IAgent& agent, ISessionManager& session, ISettingsManager& settings,
                   AutoRetryController& retry, CompactionController& compaction,
                   RecoveryAttemptOmitter& omitter, ISessionEventSink& sink);

    /**
     * last is the final assistant message of the run. Returns true when the caller should
     * continue the agent (retry, overflow recovery or queued messages).
     */
    bool afterRun(const std::optional<AssistantMessage>& last, const std::vector<ToolResultMessage>& toolResults,
                  const std::function<bool()>& abortRequested);

    /** The compaction check alone, also run before a prompt for an aborted last response. */
    bool checkCompaction(const AssistantMessage& message, bool skipAbortedCheck,
                         const std::vector<ToolResultMessage>& toolResults);

    /**
     * The transcript for the next model request: the session's projection, compacted first when
     * its estimated size crossed the threshold.
     */
    std::vector<AgentMessage> contextForNextResponse();

    /** A new user message or a healthy response ends overflow recovery. */
    void resetOverflowRecovery();

private:
    bool applyOverflow(const CompactionDecision& decision, const AssistantMessage& message,
                       const std::vector<ToolResultMessage>& toolResults);
    void reportRecoveryExhausted(const CompactionDecision& decision);

    IAgent& m_agent;
    ISessionManager& m_session;
    ISettingsManager& m_settings;
    AutoRetryController& m_retry;
    CompactionController& m_compaction;
    RecoveryAttemptOmitter& m_omitter;
    ISessionEventSink& m_sink;
    CompactionTrigger m_trigger;
    AgentTokenEstimator m_estimator;
    MessageEntryLocator m_locator;
    std::atomic<bool> m_overflowRecoveryAttempted{false};
};

PostRunHandler::PostRunHandler(IAgent& agent, ISessionManager& session, ISettingsManager& settings,
                               AutoRetryController& retry, CompactionController& compaction,
                               RecoveryAttemptOmitter& omitter, ISessionEventSink& sink)
    : m_agent(agent),
      m_session(session),
      m_settings(settings),
      m_retry(retry),
      m_compaction(compaction),
      m_omitter(omitter),
      m_sink(sink) {}

void PostRunHandler::resetOverflowRecovery() {
    m_overflowRecoveryAttempted = false;
}

void PostRunHandler::reportRecoveryExhausted(const CompactionDecision& decision) {
    AgentSessionEvent event;
    event.type = SessionEventType::CompactionEnd;
    event.reason = "overflow";
    event.errorMessage = decision.errorMessage;
    m_sink.emit(event);
}

bool PostRunHandler::applyOverflow(const CompactionDecision& decision, const AssistantMessage& message,
                                   const std::vector<ToolResultMessage>& toolResults) {
    if (decision.action == CompactionAction::OverflowNoRetry) {
        return m_compaction.runAuto("overflow", false);
    }
    if (decision.action == CompactionAction::OverflowRecoveryExhausted) {
        reportRecoveryExhausted(decision);
        return false;
    }
    // Keep the failed final attempt out of context before compacting and running the turn again.
    m_overflowRecoveryAttempted = true;
    if (const auto omitted = m_omitter.omit(message, toolResults); !omitted) {
        return false;
    }
    return m_compaction.runAuto("overflow", true);
}

bool PostRunHandler::checkCompaction(const AssistantMessage& message, bool skipAbortedCheck,
                                     const std::vector<ToolResultMessage>& toolResults) {
    const Model model = m_agent.model();
    const CompactionSettings settings = m_settings.view().compactionSettings(model.provider, model.id);
    const std::vector<SessionEntry> branch = m_session.branchPath();
    const SessionProjection projection = m_session.buildSessionProjection();
    const std::vector<AgentMessage> messages = m_agent.messages();
    const std::optional<std::string> entryId = m_locator.find(branch, AgentMessage(message));
    const CompactionCheck check{message,
                                skipAbortedCheck,
                                settings,
                                message.provider == model.provider && message.model == model.id,
                                model.contextWindow,
                                model.maxTokens,
                                entryId,
                                branch,
                                projection,
                                messages,
                                m_overflowRecoveryAttempted.load()};
    const CompactionDecision decision = m_trigger.decide(check);
    switch (decision.action) {
    case CompactionAction::None:
        return false;
    case CompactionAction::Threshold:
        return m_compaction.runAuto("threshold", false);
    default:
        return applyOverflow(decision, message, toolResults);
    }
}

std::vector<AgentMessage> PostRunHandler::contextForNextResponse() {
    const SessionProjection projection = m_session.buildSessionProjection();
    const Model model = m_agent.model();
    if (model.id.empty() || model.contextWindow <= 0) {
        return projection.messages;
    }
    const CompactionSettings settings = m_settings.view().compactionSettings(model.provider, model.id);
    const std::int64_t tokens = m_estimator.estimateProjected(projection, m_session.branchPath()).tokens;
    if (!m_estimator.shouldCompact(tokens, model.contextWindow, settings)) {
        return projection.messages;
    }
    m_compaction.runAuto("threshold", false);
    return m_session.buildSessionProjection().messages;
}

bool PostRunHandler::afterRun(const std::optional<AssistantMessage>& last,
                              const std::vector<ToolResultMessage>& toolResults,
                              const std::function<bool()>& abortRequested) {
    if (abortRequested()) {
        m_retry.finishCancelled();
        return false;
    }
    if (!last) {
        return m_agent.hasQueuedMessages();
    }
    const std::int64_t window = m_agent.model().contextWindow;
    if (m_retry.isRetryable(*last, window) && m_retry.prepareRetry(*last)) {
        if (abortRequested()) {
            m_retry.finishCancelled();
            return false;
        }
        return true;
    }
    if (abortRequested()) {
        m_retry.finishCancelled();
        return false;
    }
    if (last->stopReason == StopReason::Error && m_retry.attempt() > 0) {
        m_retry.failed(*last);
    }
    if (checkCompaction(*last, true, toolResults)) {
        return !abortRequested();
    }
    return !abortRequested() && m_agent.hasQueuedMessages();
}
