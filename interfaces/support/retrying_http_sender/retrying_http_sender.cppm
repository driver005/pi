export module pi.support.retrying_http_sender;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
export import pi.support.provider_error_formatter;
export import pi.support.retry_policy;

/**
 * Sends a provider request with the SDK retry policy and an abortable backoff. Providers call
 * this instead of IHttpClient::send so retry behavior is identical across wire APIs.
 * A final non-2xx response is returned as a normal HttpResponse for the caller to report.
 */
export class RetryingHttpSender {
public:
    RetryingHttpSender(IHttpClient& http, ISleeper& sleeper, const IClock& clock)
        : m_http(http),
          m_sleeper(sleeper),
          m_clock(clock) {}

    Result<HttpResponse> send(const HttpRequest& request, int maxRetries, std::optional<std::int64_t> maxRetryDelayMs) {
        for (int attempt = 0;; ++attempt) {
            auto result = m_http.send(request);
            const bool aborted = request.signal && request.signal->aborted();
            if (aborted) {
                return std::unexpected(abortError());
            }
            const bool retriesLeft = attempt < maxRetries;
            if (result.has_value()) {
                const bool failed = result->status < 200 || result->status >= 300;
                if (!failed || !retriesLeft || !m_policy.isRetryableStatus(*result)) {
                    return result;
                }
                auto waited = backOff(request, *result, attempt, maxRetryDelayMs);
                if (!waited) {
                    return std::unexpected(waited.error());
                }
                if (!*waited) {
                    return std::unexpected(abortError());
                }
                continue;
            }
            if (!retriesLeft || !m_policy.isRetryableTransportError(result.error())) {
                return result;
            }
            const auto wait = std::chrono::milliseconds(m_policy.backoffMs(attempt, jitter()));
            if (!m_sleeper.sleep(wait, request.signal)) {
                return std::unexpected(abortError());
            }
        }
    }

private:
    Result<bool> backOff(const HttpRequest& request, const HttpResponse& response, int retryIndex, std::optional<std::int64_t> maxRetryDelayMs) {
        const std::string message = m_formatter.formatHttp(response);
        auto delay = m_policy.delayMs(response, retryIndex, maxRetryDelayMs, m_clock.nowMs(), jitter(),
                                      message);
        if (!delay) {
            return std::unexpected(delay.error());
        }
        const auto wait = std::chrono::milliseconds(std::max<std::int64_t>(0, *delay));
        return m_sleeper.sleep(wait, request.signal);
    }

    double jitter() {
        return std::uniform_real_distribution<double>(0.0, 1.0)(m_random);
    }

    Error abortError() const {
        return Error{"aborted", "Request aborted"};
    }

    IHttpClient& m_http;
    ISleeper& m_sleeper;
    const IClock& m_clock;
    RetryPolicy m_policy;
    ProviderErrorFormatter m_formatter;
    std::mt19937 m_random{std::random_device{}()};
};
