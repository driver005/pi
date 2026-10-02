module;

#include <cstdint>

export module pi.support.assistant_call_retrier;

import std;
export import pi.platform.i_sleeper;
export import pi.support.abort_signal;
export import pi.support.assistant_error_classifier;
export import pi.types.assistant_message;
export import pi.types.assistant_retry_policy;
export import pi.types.retry_callbacks;

/**
 * Runs one assistant-producing call with bounded retry on transient errors and exponential
 * backoff. Aborts are terminal and never retried (also during the backoff sleep); deterministic
 * errors return at once. Port of retryAssistantCall in utils/retry.ts.
 */
export class AssistantCallRetrier {
public:
    explicit AssistantCallRetrier(ISleeper& sleeper);

    using Produce = std::function<AssistantMessage()>;

    AssistantMessage run(const Produce& produce, const AssistantRetryPolicy& policy,
                         const std::shared_ptr<AbortSignal>& signal,
                         const RetryCallbacks& callbacks) const;

    /** baseDelayMs * 2^(attempt-1), capped at maxDelayMs. */
    std::int64_t delayMs(const AssistantRetryPolicy& policy, int attempt) const;

private:
    AssistantMessage abortedCopy(const AssistantMessage& response) const;

    ISleeper& m_sleeper;
    AssistantErrorClassifier m_classifier;
};

AssistantCallRetrier::AssistantCallRetrier(ISleeper& sleeper) : m_sleeper(sleeper) {}

std::int64_t AssistantCallRetrier::delayMs(const AssistantRetryPolicy& policy, int attempt) const {
    std::int64_t delay = policy.baseDelayMs;
    for (int i = 1; i < attempt && delay < policy.maxDelayMs; ++i) {
        delay *= 2;
    }
    return std::min(delay, policy.maxDelayMs);
}

AssistantMessage AssistantCallRetrier::abortedCopy(const AssistantMessage& response) const {
    AssistantMessage copy = response;
    copy.errorMessage.reset();
    copy.stopReason = StopReason::Aborted;
    return copy;
}

AssistantMessage AssistantCallRetrier::run(const Produce& produce, const AssistantRetryPolicy& policy,
                                           const std::shared_ptr<AbortSignal>& signal,
                                           const RetryCallbacks& callbacks) const {
    const int maxAttempts = policy.enabled ? policy.maxRetries : 0;
    int attempt = 0;
    std::optional<std::string> lastError;
    const auto finished = [&](bool success, const std::optional<std::string>& finalError) {
        if (lastError && callbacks.onRetryFinished) {
            callbacks.onRetryFinished(success, attempt, finalError);
        }
    };
    while (true) {
        AssistantMessage response = produce();
        if (response.stopReason == StopReason::Aborted) {
            finished(false, std::nullopt);
            return response;
        }
        if (response.stopReason != StopReason::Error) {
            finished(true, std::nullopt);
            return response;
        }
        if (attempt >= maxAttempts || !m_classifier.isRetryable(response)) {
            finished(false, response.errorMessage);
            return response;
        }
        ++attempt;
        lastError = response.errorMessage.value_or("Unknown error");
        const std::int64_t delay = delayMs(policy, attempt);
        if (callbacks.onRetryScheduled) {
            callbacks.onRetryScheduled(attempt, maxAttempts, delay, *lastError);
        }
        if (!m_sleeper.sleep(std::chrono::milliseconds(delay), signal)) {
            finished(false, lastError);
            return abortedCopy(response);
        }
        if (callbacks.onRetryAttemptStart) {
            callbacks.onRetryAttemptStart();
        }
    }
}
