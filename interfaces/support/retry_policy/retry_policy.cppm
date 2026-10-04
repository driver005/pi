export module pi.support.retry_policy;

import std;
export import pi.support.header_merger;
export import pi.support.http_date_parser;
export import pi.types.http_response;
export import pi.types.result;

/**
 * Which failed requests are retried and how long to wait. Mirrors the OpenAI/Anthropic SDK
 * policy: 408, 409, 429 and 5xx are retried unless x-should-retry says otherwise; the server's
 * retry-after(-ms) wins over exponential backoff but is refused when it exceeds the cap.
 * Port of utils/provider-retry.ts.
 */
export class RetryPolicy {
public:
    static constexpr std::int64_t DefaultMaxRetryDelayMs = 60000;

    bool isRetryableStatus(const HttpResponse& response) const {
        const auto shouldRetry = m_headers.find(response.headers, "x-should-retry");
        if (shouldRetry == "true") {
            return true;
        }
        if (shouldRetry == "false") {
            return false;
        }
        return response.status == 408 || response.status == 409 || response.status == 429 ||
               response.status >= 500;
    }

    /** Transport failures are retried except aborts. */
    bool isRetryableTransportError(const Error& error) const {
        return error.code != "aborted";
    }

    /**
     * Delay before retry number retryIndex (0-based). jitter is a random value in [0, 1).
     * Error when the server asks for longer than maxRetryDelayMs (0 disables the cap).
     */
    Result<std::int64_t> delayMs(const HttpResponse& response, int retryIndex, std::optional<std::int64_t> maxRetryDelayMs, std::int64_t nowMs, double jitter, const std::string& providerMessage) const {
        if (const auto header = m_headers.find(response.headers, "retry-after-ms")) {
            if (const auto value = parseNumber(*header)) {
                return checkServerDelay(static_cast<std::int64_t>(*value), maxRetryDelayMs,
                                        providerMessage);
            }
        }
        if (const auto header = m_headers.find(response.headers, "retry-after")) {
            if (const auto seconds = parseNumber(*header)) {
                return checkServerDelay(static_cast<std::int64_t>(*seconds * 1000.0), maxRetryDelayMs,
                                        providerMessage);
            }
            if (const auto date = m_dates.parseMs(*header)) {
                return checkServerDelay(*date - nowMs, maxRetryDelayMs, providerMessage);
            }
        }
        return backoffMs(retryIndex, jitter);
    }

    std::int64_t backoffMs(int retryIndex, double jitter) const {
        const double seconds = std::min(0.5 * std::pow(2.0, retryIndex), 8.0);
        return static_cast<std::int64_t>(seconds * 1000.0 * (1.0 - jitter * 0.25));
    }

private:
    std::optional<double> parseNumber(const std::string& text) const {
        const char* begin = text.c_str();
        char* end = nullptr;
        const double value = std::strtod(begin, &end);
        if (end == begin || !std::isfinite(value)) {
            return std::nullopt;
        }
        return value;
    }

    Result<std::int64_t> checkServerDelay(std::int64_t delayMs, std::optional<std::int64_t> maxRetryDelayMs, const std::string& providerMessage) const {
        const std::int64_t maxDelay = maxRetryDelayMs.value_or(DefaultMaxRetryDelayMs);
        if (maxDelay > 0 && delayMs > maxDelay) {
            return std::unexpected(Error{
                "retry_delay_exceeded",
                "Server requested " + std::to_string((delayMs + 999) / 1000) + "s retry delay (max: " +
                    std::to_string((maxDelay + 999) / 1000) + "s). " + providerMessage});
        }
        return delayMs;
    }

    HeaderMerger m_headers;
    HttpDateParser m_dates;
};
