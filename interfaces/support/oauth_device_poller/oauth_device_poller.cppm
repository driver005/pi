module;

#include <cstdint>

export module pi.support.oauth_device_poller;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_sleeper;
export import pi.support.abort_signal;
export import pi.types.device_poll_result;
export import pi.types.result;

/**
 * Polls a device flow until it completes, fails, expires or is cancelled. The interval is at least one second and 5 seconds by
 * default (RFC 8628, 3.2); `slow_down` takes the interval the server names or adds 5 seconds (3.5). Port of
 * pollOAuthDeviceCodeFlow in packages/ai/src/auth/oauth/device-code.ts. Errors: "login_cancelled", "device_timeout" (the
 * message names clock drift when the server asked to slow down), and the failure message of a `failed` poll as "oauth".
 */
export class OauthDevicePoller {
public:
    OauthDevicePoller(const IClock& clock, ISleeper& sleeper)
        : m_clock(clock),
          m_sleeper(sleeper) {}

    Result<Json> poll(std::optional<std::int64_t> intervalSeconds, std::optional<std::int64_t> expiresInSeconds, bool waitBeforeFirstPoll, const std::shared_ptr<AbortSignal>& signal, const std::function<DevicePollResult()>& attempt) const {
        const std::int64_t deadline = expiresInSeconds ? m_clock.nowMs() + *expiresInSeconds * 1000 : std::numeric_limits<std::int64_t>::max();
        std::int64_t intervalMs = std::max<std::int64_t>(1000, intervalSeconds.value_or(5) * 1000);
        int slowDowns = 0;
        if (waitBeforeFirstPoll) {
            if (auto waited = wait(std::min(intervalMs, deadline - m_clock.nowMs()), signal); !waited) {
                return std::unexpected(waited.error());
            }
        }
        while (m_clock.nowMs() < deadline) {
            if (signal && signal->aborted()) {
                return std::unexpected(Error{"login_cancelled", "Login cancelled"});
            }
            DevicePollResult result = attempt();
            if (result.status == "complete") {
                return result.value;
            }
            if (result.status == "failed") {
                return std::unexpected(Error{"oauth", result.message});
            }
            if (result.status == "slow_down") {
                ++slowDowns;
                intervalMs = result.intervalSeconds && *result.intervalSeconds > 0 ? std::max<std::int64_t>(1000, *result.intervalSeconds * 1000) : std::max<std::int64_t>(1000, intervalMs + 5000);
            }
            const std::int64_t remaining = deadline - m_clock.nowMs();
            if (remaining <= 0) {
                break;
            }
            if (auto waited = wait(std::min(intervalMs, remaining), signal); !waited) {
                return std::unexpected(waited.error());
            }
        }
        return std::unexpected(Error{"device_timeout", slowDowns > 0 ? "Device flow timed out after one or more slow_down responses. This is often caused by clock drift in WSL or VM environments. Please sync or restart the VM clock and try again." : "Device flow timed out"});
    }

private:
    Result<void> wait(std::int64_t ms, const std::shared_ptr<AbortSignal>& signal) const {
        if (ms > 0 && !m_sleeper.sleep(std::chrono::milliseconds(ms), signal)) {
            return std::unexpected(Error{"login_cancelled", "Login cancelled"});
        }
        return {};
    }

    const IClock& m_clock;
    ISleeper& m_sleeper;
};
