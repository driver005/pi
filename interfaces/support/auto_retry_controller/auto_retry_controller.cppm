module;

#include <cstdint>

export module pi.support.auto_retry_controller;

import std;
export import pi.platform.i_sleeper;
export import pi.session.i_session_event_sink;
export import pi.session.i_settings_manager;
export import pi.support.assistant_call_retrier;
export import pi.support.assistant_error_classifier;
export import pi.support.overflow_detector;
export import pi.support.recovery_attempt_omitter;
export import pi.types.agent_message;
export import pi.types.assistant_retry_policy;

/**
 * Retries a failed assistant turn: counts attempts against settings.retry, keeps the failed
 * attempt out of context, waits the backoff (abortable) and reports start/end events. Context
 * overflow is never retried here; compaction handles it. Port of the auto-retry section of
 * AgentSession.
 */
export class AutoRetryController {
public:
    AutoRetryController(ISettingsManager& settings, ISleeper& sleeper, RecoveryAttemptOmitter& omitter,
                        ISessionEventSink& sink);

    /** A transient error (not context overflow) of a failed assistant message. */
    bool isRetryable(const AssistantMessage& message, std::int64_t contextWindow) const;

    /** Whether the run that just ended will be retried: budget left and the last assistant failed transiently. */
    bool willRetryAfterAgentEnd(const std::vector<AgentMessage>& messages, std::int64_t contextWindow,
                                bool abortRequested) const;

    /**
     * Emits auto_retry_start, omits the failed attempt from context and sleeps the backoff.
     * True when the caller should continue the agent; false when disabled, exhausted or aborted.
     */
    bool prepareRetry(const AssistantMessage& message);

    /** A response succeeded: ends an active retry sequence successfully. */
    void succeeded();
    /** The sequence ended with a final failed response. */
    void failed(const AssistantMessage& message);
    /** Ends an active retry sequence as cancelled. */
    void finishCancelled();

    void abort();
    bool retrying() const;
    int attempt() const;

private:
    AssistantRetryPolicy policy() const;
    void emitEnd(bool success, int attempt, const std::optional<std::string>& finalError);

    ISettingsManager& m_settings;
    ISleeper& m_sleeper;
    RecoveryAttemptOmitter& m_omitter;
    ISessionEventSink& m_sink;
    AssistantErrorClassifier m_classifier;
    OverflowDetector m_overflow;
    AssistantCallRetrier m_delays;

    mutable std::mutex m_mutex;
    int m_attempt = 0;
    std::shared_ptr<AbortSignal> m_sleepSignal;
};

AutoRetryController::AutoRetryController(ISettingsManager& settings, ISleeper& sleeper,
                                         RecoveryAttemptOmitter& omitter, ISessionEventSink& sink)
    : m_settings(settings), m_sleeper(sleeper), m_omitter(omitter), m_sink(sink), m_delays(sleeper) {}

AssistantRetryPolicy AutoRetryController::policy() const {
    return m_settings.view().retryPolicy();
}

bool AutoRetryController::isRetryable(const AssistantMessage& message, std::int64_t contextWindow) const {
    if (m_overflow.isContextOverflow(message, contextWindow)) {
        return false;
    }
    return m_classifier.isRetryable(message);
}

bool AutoRetryController::willRetryAfterAgentEnd(const std::vector<AgentMessage>& messages,
                                                 std::int64_t contextWindow, bool abortRequested) const {
    const AssistantRetryPolicy settings = policy();
    if (abortRequested || !settings.enabled || attempt() >= settings.maxRetries) {
        return false;
    }
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (const auto* assistant = std::get_if<AssistantMessage>(&*it)) {
            return isRetryable(*assistant, contextWindow);
        }
    }
    return false;
}

void AutoRetryController::emitEnd(bool success, int attempt, const std::optional<std::string>& finalError) {
    AgentSessionEvent event;
    event.type = SessionEventType::AutoRetryEnd;
    event.success = success;
    event.attempt = attempt;
    event.finalError = finalError;
    m_sink.emit(event);
}

bool AutoRetryController::prepareRetry(const AssistantMessage& message) {
    const AssistantRetryPolicy settings = policy();
    if (!settings.enabled) {
        return false;
    }
    int attempt = 0;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_attempt + 1 > settings.maxRetries) {
            return false;
        }
        attempt = ++m_attempt;
    }
    AgentSessionEvent start;
    start.type = SessionEventType::AutoRetryStart;
    start.attempt = attempt;
    start.maxAttempts = settings.maxRetries;
    start.delayMs = m_delays.delayMs(settings, attempt);
    start.errorMessage = message.errorMessage.value_or("Unknown error");
    m_sink.emit(start);

    // The failed attempt stays in the file but leaves the model's context.
    if (const auto omitted = m_omitter.omit(message, {}); !omitted) {
        finishCancelled();
        return false;
    }
    const auto signal = std::make_shared<AbortSignal>();
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_sleepSignal = signal;
    }
    const bool completed = m_sleeper.sleep(std::chrono::milliseconds(start.delayMs), signal);
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_sleepSignal.reset();
    }
    if (!completed) {
        finishCancelled();
    }
    return completed;
}

void AutoRetryController::succeeded() {
    int attempt = 0;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        attempt = std::exchange(m_attempt, 0);
    }
    if (attempt > 0) {
        emitEnd(true, attempt, std::nullopt);
    }
}

void AutoRetryController::failed(const AssistantMessage& message) {
    int attempt = 0;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        attempt = std::exchange(m_attempt, 0);
    }
    if (attempt > 0) {
        emitEnd(false, attempt, message.errorMessage);
    }
}

void AutoRetryController::finishCancelled() {
    int attempt = 0;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        attempt = std::exchange(m_attempt, 0);
    }
    if (attempt > 0) {
        emitEnd(false, attempt, std::string("Retry cancelled"));
    }
}

void AutoRetryController::abort() {
    std::shared_ptr<AbortSignal> signal;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        signal = m_sleepSignal;
    }
    if (signal) {
        signal->abort();
    }
}

bool AutoRetryController::retrying() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_sleepSignal != nullptr;
}

int AutoRetryController::attempt() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_attempt;
}
