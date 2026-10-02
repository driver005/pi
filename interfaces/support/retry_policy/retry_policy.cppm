export module pi.support.retry_policy;

import std;
export import pi.support.header_merger;
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

    bool isRetryableStatus(const HttpResponse& response) const;

    /** Transport failures are retried except aborts. */
    bool isRetryableTransportError(const Error& error) const;

    /**
     * Delay before retry number retryIndex (0-based). jitter is a random value in [0, 1).
     * Error when the server asks for longer than maxRetryDelayMs (0 disables the cap).
     */
    Result<std::int64_t> delayMs(const HttpResponse& response, int retryIndex,
                                 std::optional<std::int64_t> maxRetryDelayMs, std::int64_t nowMs,
                                 double jitter, const std::string& providerMessage) const;

    std::int64_t backoffMs(int retryIndex, double jitter) const;

private:
    std::optional<double> parseNumber(const std::string& text) const;
    std::optional<std::int64_t> parseHttpDateMs(const std::string& text) const;
    Result<std::int64_t> checkServerDelay(std::int64_t delayMs,
                                          std::optional<std::int64_t> maxRetryDelayMs,
                                          const std::string& providerMessage) const;

    HeaderMerger m_headers;
};

bool RetryPolicy::isRetryableStatus(const HttpResponse& response) const {
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

bool RetryPolicy::isRetryableTransportError(const Error& error) const {
    return error.code != "aborted";
}

std::optional<double> RetryPolicy::parseNumber(const std::string& text) const {
    const char* begin = text.c_str();
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> RetryPolicy::parseHttpDateMs(const std::string& text) const {
    // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
    const std::string months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const auto comma = text.find(',');
    if (comma == std::string::npos || text.size() < comma + 21) {
        return std::nullopt;
    }
    std::istringstream stream(text.substr(comma + 1));
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    char colon = 0;
    std::string monthName;
    stream >> day >> monthName >> year >> hour >> colon >> minute >> colon >> second;
    const auto month = months.find(monthName);
    if (stream.fail() || monthName.size() != 3 || month == std::string::npos || month % 3 != 0) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{year},
                                           std::chrono::month{static_cast<unsigned>(month / 3 + 1)},
                                           std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const auto seconds = std::chrono::sys_days{date}.time_since_epoch() + std::chrono::hours{hour} +
                         std::chrono::minutes{minute} + std::chrono::seconds{second};
    return std::chrono::duration_cast<std::chrono::milliseconds>(seconds).count();
}

Result<std::int64_t> RetryPolicy::checkServerDelay(std::int64_t delayMs,
                                                   std::optional<std::int64_t> maxRetryDelayMs,
                                                   const std::string& providerMessage) const {
    const std::int64_t maxDelay = maxRetryDelayMs.value_or(DefaultMaxRetryDelayMs);
    if (maxDelay > 0 && delayMs > maxDelay) {
        return std::unexpected(Error{
            "retry_delay_exceeded",
            "Server requested " + std::to_string((delayMs + 999) / 1000) + "s retry delay (max: " +
                std::to_string((maxDelay + 999) / 1000) + "s). " + providerMessage});
    }
    return delayMs;
}

std::int64_t RetryPolicy::backoffMs(int retryIndex, double jitter) const {
    const double seconds = std::min(0.5 * std::pow(2.0, retryIndex), 8.0);
    return static_cast<std::int64_t>(seconds * 1000.0 * (1.0 - jitter * 0.25));
}

Result<std::int64_t> RetryPolicy::delayMs(const HttpResponse& response, int retryIndex,
                                          std::optional<std::int64_t> maxRetryDelayMs,
                                          std::int64_t nowMs, double jitter,
                                          const std::string& providerMessage) const {
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
        if (const auto date = parseHttpDateMs(*header)) {
            return checkServerDelay(*date - nowMs, maxRetryDelayMs, providerMessage);
        }
    }
    return backoffMs(retryIndex, jitter);
}
