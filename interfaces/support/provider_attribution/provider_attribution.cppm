export module pi.support.provider_attribution;

import std;
export import pi.types.model;
import pi.support.url_parser;

/**
 * The headers pi adds to requests for providers that attribute traffic: OpenRouter, NVIDIA NIM and Cloudflare get their
 * attribution headers while install telemetry is on, and opencode gets its session headers. Headers a caller already set win.
 * Port of core/provider-attribution.ts.
 */
export class ProviderAttribution {
public:
    using Headers = std::vector<std::pair<std::string, std::optional<std::string>>>;

    /** Session and attribution headers first, then each source over them (a nullopt value removes a header). */
    Headers merge(const Model& model, bool installTelemetry, const std::optional<std::string>& sessionId, const std::vector<Headers>& sources) const {
        Headers merged = sessionHeaders(model, sessionId);
        if (installTelemetry) {
            for (auto& header : attributionHeaders(model)) {
                set(merged, header.first, header.second);
            }
        }
        for (const Headers& source : sources) {
            for (const auto& header : source) {
                set(merged, header.first, header.second);
            }
        }
        return merged;
    }

private:
    Headers attributionHeaders(const Model& model) const {
        if (model.provider == "openrouter" || model.baseUrl.find("openrouter.ai") != std::string::npos) {
            return {{"HTTP-Referer", "https://pi.dev"}, {"X-OpenRouter-Title", "pi"}, {"X-OpenRouter-Categories", "cli-agent"}};
        }
        if (model.provider == "nvidia" || matchesHost(model.baseUrl, "integrate.api.nvidia.com")) {
            return {{"X-BILLING-INVOKE-ORIGIN", "Pi"}};
        }
        if (model.provider == "cloudflare-workers-ai" || model.provider == "cloudflare-ai-gateway" || matchesHost(model.baseUrl, "api.cloudflare.com") || matchesHost(model.baseUrl, "gateway.ai.cloudflare.com")) {
            return {{"User-Agent", "pi-coding-agent"}};
        }
        return {};
    }

    Headers sessionHeaders(const Model& model, const std::optional<std::string>& sessionId) const {
        if (!sessionId || sessionId->empty()) {
            return {};
        }
        if (model.provider != "opencode" && model.provider != "opencode-go" && !matchesHost(model.baseUrl, "opencode.ai")) {
            return {};
        }
        return {{"x-opencode-session", *sessionId}, {"x-opencode-client", "pi"}};
    }

    bool matchesHost(const std::string& baseUrl, const std::string& expected) const {
        const auto parsed = m_urls.parse(baseUrl);
        return parsed && parsed->host == expected;
    }

    void set(Headers& headers, const std::string& name, const std::optional<std::string>& value) const {
        const auto found = std::ranges::find(headers, name, &Headers::value_type::first);
        if (found != headers.end()) {
            found->second = value;
        } else {
            headers.emplace_back(name, value);
        }
    }

    UrlParser m_urls;
};
