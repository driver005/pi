export module pi.support.settings_resolver;

import std;
export import pi.types.harness_run_settings;
export import pi.types.resolved_settings;

/** Resolves the host's run settings over the built-in defaults: partial policies merge field by field. */
export class SettingsResolver {
public:
    ResolvedSettings resolve(const HarnessRunSettings& settings) const {
        ResolvedSettings resolved;
        resolved.extensions = settings.extensions;
        resolved.stream = settings.stream;
        const Json& retry = settings.retry;
        resolved.retry.enabled = retry.value("enabled", resolved.retry.enabled);
        resolved.retry.maxRetries = retry.value("maxRetries", resolved.retry.maxRetries);
        resolved.retry.baseDelayMs = retry.value("baseDelayMs", resolved.retry.baseDelayMs);
        if (retry.contains("maxAgentDelayMs")) {
            resolved.retry.maxAgentDelayMs = retry.at("maxAgentDelayMs").is_null()
                                                 ? std::nullopt
                                                 : std::optional<std::int64_t>(retry.at("maxAgentDelayMs").get<std::int64_t>());
        }
        const Json& compaction = settings.compaction;
        resolved.compaction.enabled = compaction.value("enabled", resolved.compaction.enabled);
        resolved.compaction.reserveTokens = compaction.value("reserveTokens", resolved.compaction.reserveTokens);
        resolved.compaction.keepRecentTokens = compaction.value("keepRecentTokens", resolved.compaction.keepRecentTokens);
        resolved.compaction.backgroundTokens = compaction.value("backgroundTokens", resolved.compaction.backgroundTokens);
        resolved.toolExecution = settings.toolExecution.value_or(resolved.toolExecution);
        resolved.steeringMode = settings.steeringMode.value_or(resolved.steeringMode);
        resolved.followUpMode = settings.followUpMode.value_or(resolved.followUpMode);
        return resolved;
    }
};
