module;

#include <cstdint>

export module pi.types.login_interaction;

import std;
export import pi.support.abort_signal;

/**
 * How a sign-in talks to the person signing in: the callbacks show what to do (open the authorization URL, enter the device
 * code at the verification URL) and report progress; `manualCode` asks for a pasted authorization code or redirect URL when
 * the browser cannot reach the loopback callback server. Callbacks that are not set are skipped.
 */
export struct LoginInteraction {
    std::function<void(const std::string& url, const std::string& instructions)> authUrl;
    std::function<void(const std::string& userCode, const std::string& verificationUri, std::optional<std::int64_t> intervalSeconds, std::optional<std::int64_t> expiresInSeconds)> deviceCode;
    std::function<void(const std::string& message)> progress;
    /** Reads a pasted code or redirect URL; nullopt when nothing was entered. */
    std::function<std::optional<std::string>(const std::string& message)> manualCode;
    /** Skips the loopback callback server and asks for the code, for a browser on another machine. */
    bool manualOnly = false;
    std::shared_ptr<AbortSignal> signal;
};
