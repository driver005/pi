module;

#include <cstdint>

export module pi.support.provider_login_specs;

import std;
export import pi.types.browser_login_spec;
export import pi.types.device_login_spec;

/**
 * How each subscription provider signs in: the method ids a provider offers ("browser", "copy_code", "device_code"; the first
 * is the default) and the browser and device descriptions of the flows that follow the standard shapes. Constants from the
 * TypeScript oauth modules in packages/ai/src/auth/oauth. Not ported: the Meta and Radius sign-ins, GitHub Enterprise for
 * Copilot (github.com only).
 */
export class ProviderLoginSpecs {
public:
    /** Provider ids that can sign in, in the order they are listed. */
    std::vector<std::string> providers() const {
        return {"anthropic", "openai-codex", "openai", "xai", "kimi-coding", "openrouter", "github-copilot"};
    }

    std::vector<std::string> methods(const std::string& provider) const {
        if (provider == "anthropic") {
            return {"browser", "copy_code"};
        }
        if (provider == "openai-codex") {
            return {"browser", "device_code"};
        }
        if (provider == "openai" || provider == "openrouter") {
            return {"browser"};
        }
        if (provider == "xai" || provider == "kimi-coding" || provider == "github-copilot") {
            return {"device_code"};
        }
        return {};
    }

    std::optional<BrowserLoginSpec> browser(const std::string& provider) const {
        BrowserLoginSpec spec;
        if (provider == "anthropic") {
            spec.authorizeUrl = "https://claude.ai/oauth/authorize";
            spec.clientId = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
            spec.scope = "org:create_api_key user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload";
            spec.port = 53692;
            spec.pasteWhenPortIsTaken = true;
            spec.stateIsVerifier = true;
            spec.authorizeParams = {{"code", "true"}};
            spec.stateInTokenRequest = true;
            spec.copyCodeRedirectUri = "https://platform.claude.com/oauth/code/callback";
            spec.providerName = "Anthropic";
            return spec;
        }
        if (provider == "openai-codex") {
            spec.authorizeUrl = "https://auth.openai.com/oauth/authorize";
            spec.clientId = "app_EMoamEEZ73f0CkXaXp7hrann";
            spec.scope = "openid profile email offline_access";
            spec.port = 1455;
            spec.path = "/auth/callback";
            spec.pasteWhenPortIsTaken = true;
            spec.authorizeParams = {{"id_token_add_organizations", "true"}, {"codex_cli_simplified_flow", "true"}, {"originator", "pi"}};
            spec.providerName = "OpenAI";
            return spec;
        }
        if (provider == "openai") {
            spec.authorizeUrl = "https://auth.openai.com/api/accounts/authorize";
            spec.clientId = "dynamic_agent_client";
            spec.scope = "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct";
            spec.redirectHost = "127.0.0.1";
            spec.port = 1455;
            spec.path = "/auth/callback";
            spec.authorizeParams = {{"agent_name_hint", "Pi"}, {"resource", "https://api.openai.com/v1"}};
            spec.dynamicClientId = true;
            spec.requireIdToken = true;
            spec.providerName = "ChatGPT";
            return spec;
        }
        if (provider == "openrouter") {
            spec.authorizeUrl = "https://openrouter.ai/auth";
            spec.redirectHost = "127.0.0.1";
            spec.path = "/oauth/callback";
            spec.randomPath = true;
            spec.noState = true;
            spec.sendClientId = false;
            spec.redirectParam = "callback_url";
            spec.exchangesForKey = true;
            spec.exchangeUrl = "https://openrouter.ai/api/v1/auth/keys";
            spec.providerName = "OpenRouter";
            return spec;
        }
        return std::nullopt;
    }

    /** `kimiHost` overrides the Kimi OAuth host (KIMI_CODE_OAUTH_HOST / KIMI_OAUTH_HOST). */
    std::optional<DeviceLoginSpec> device(const std::string& provider, const std::string& kimiHost = "") const {
        DeviceLoginSpec spec;
        if (provider == "xai") {
            spec.name = "xAI";
            spec.deviceUrl = "https://auth.x.ai/oauth2/device/code";
            spec.tokenUrl = "https://auth.x.ai/oauth2/token";
            spec.clientId = "b1a00492-073a-47ea-816f-4c329264a828";
            spec.scope = "openid profile email offline_access grok-cli:access api:access";
            spec.deviceParams = {{"referrer", "pi"}};
            spec.preferCompleteUri = true;
            spec.httpsOnly = true;
            return spec;
        }
        if (provider == "kimi-coding") {
            std::string host = kimiHost.empty() ? "https://auth.kimi.com" : kimiHost;
            while (!host.empty() && host.back() == '/') {
                host.pop_back();
            }
            spec.name = "Kimi Code";
            spec.deviceUrl = host + "/api/oauth/device_authorization";
            spec.tokenUrl = host + "/api/oauth/token";
            spec.clientId = "17e5f671-d194-4dfb-9706-5516cb48c098";
            spec.preferCompleteUri = true;
            spec.requireCompleteUri = true;
            spec.defaultExpiresSeconds = 15 * 60;
            spec.serverErrorFails = true;
            return spec;
        }
        if (provider == "github-copilot") {
            spec.name = "GitHub Copilot";
            spec.deviceUrl = "https://github.com/login/device/code";
            spec.tokenUrl = "https://github.com/login/oauth/access_token";
            spec.clientId = "Iv1.b507a08c87ecfe98";
            spec.scope = "read:user";
            spec.headers = {{"User-Agent", "GitHubCopilotChat/0.35.0"}};
            spec.waitBeforeFirstPoll = false;
            return spec;
        }
        return std::nullopt;
    }
};
