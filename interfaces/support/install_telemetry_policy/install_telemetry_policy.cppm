export module pi.support.install_telemetry_policy;

import std;
export import pi.support.settings_view;

/**
 * Whether the anonymous install telemetry applies (the attribution headers for OpenRouter, NVIDIA NIM and Cloudflare; pi sends
 * no other telemetry). A set PI_TELEMETRY decides (`1`, `true` or `yes`, in any case, enable it; anything else disables);
 * without it the `enableInstallTelemetry` setting does (default on). Port of core/telemetry.ts.
 */
export class InstallTelemetryPolicy {
public:
    bool enabled(const SettingsView& settings, const std::optional<std::string>& telemetryEnv) const {
        if (!telemetryEnv) {
            return settings.enableInstallTelemetry();
        }
        std::string lower = *telemetryEnv;
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower == "1" || lower == "true" || lower == "yes";
    }
};
