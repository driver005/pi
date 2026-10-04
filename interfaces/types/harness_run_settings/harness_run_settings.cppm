export module pi.types.harness_run_settings;

import std;
export import pi.types.json;

/** Harness-wide run policy as the host supplies it: every field optional, over built-in defaults. */
export struct HarnessRunSettings {
    /** Default extension selection by name; absent selects every installed extension, in install order. */
    std::optional<std::vector<std::string>> extensions;
    /** Curated request options: transport, timeoutMs, maxRetries, maxRetryDelayMs, headers, metadata, cacheRetention. */
    Json stream = Json::object();
    /** A partial retry policy: enabled, maxRetries, baseDelayMs, maxAgentDelayMs. */
    Json retry = Json::object();
    /** A partial compaction policy: enabled, reserveTokens, keepRecentTokens, backgroundTokens. */
    Json compaction = Json::object();
    /** "parallel" or "sequential". */
    std::optional<std::string> toolExecution;
    /** "all" or "one-at-a-time". */
    std::optional<std::string> steeringMode;
    std::optional<std::string> followUpMode;
};
