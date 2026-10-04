export module pi.support.builtin_oauth_specs;

import std;
export import pi.types.oauth_refresh_spec;

/**
 * Refresh settings of the subscription providers whose tokens are exchanged with a plain token
 * endpoint: Anthropic (Claude Pro/Max), OpenAI Codex, Sign in with ChatGPT, xAI, Kimi Code and
 * OpenRouter (long-lived key); Radius is built for a gateway. Constants from the TypeScript oauth modules in packages/ai/src/auth.
 */
export class BuiltinOauthSpecs {
public:
    /** kimiHost overrides the Kimi OAuth host (KIMI_CODE_OAUTH_HOST / KIMI_OAUTH_HOST). */
    std::vector<OauthRefreshSpec> all(const std::string& kimiHost = "") const {
        OauthRefreshSpec anthropic;
        anthropic.providerId = "anthropic";
        anthropic.name = "Anthropic (Claude Pro/Max)";
        anthropic.tokenUrl = "https://platform.claude.com/v1/oauth/token";
        anthropic.jsonBody = true;
        anthropic.clientId = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
        anthropic.expiryMarginMs = 5 * 60 * 1000;

        OauthRefreshSpec codex;
        codex.providerId = "openai-codex";
        codex.name = "OpenAI (ChatGPT Plus/Pro)";
        codex.tokenUrl = "https://auth.openai.com/oauth/token";
        codex.clientId = "app_EMoamEEZ73f0CkXaXp7hrann";
        codex.accountIdFromJwt = true;

        OauthRefreshSpec chatgpt;
        chatgpt.providerId = "openai";
        chatgpt.name = "OpenAI (ChatGPT subscription)";
        chatgpt.tokenUrl = "https://auth.openai.com/api/accounts/oauth/token";
        chatgpt.missingClientIdMessage =
            "Stored OpenAI OAuth credential does not contain an issued client ID; reconnect ChatGPT";
        chatgpt.extraParams = {{"resource", "https://api.openai.com/v1"}};
        chatgpt.expiryMarginMs = 3 * 60 * 1000;
        chatgpt.requiredScope = "chatgpt.tokens.use.direct";

        OauthRefreshSpec xai;
        xai.providerId = "xai";
        xai.name = "xAI (Grok/X subscription)";
        xai.tokenUrl = "https://auth.x.ai/oauth2/token";
        xai.clientId = "b1a00492-073a-47ea-816f-4c329264a828";
        xai.keepRefreshWhenMissing = true;
        xai.expiryMarginMs = 5 * 60 * 1000;

        OauthRefreshSpec kimi;
        kimi.providerId = "kimi-coding";
        kimi.name = "Kimi Code (subscription)";
        std::string host = kimiHost.empty() ? "https://auth.kimi.com" : kimiHost;
        while (!host.empty() && host.back() == '/') {
            host.pop_back();
        }
        kimi.tokenUrl = host + "/api/oauth/token";
        kimi.clientId = "17e5f671-d194-4dfb-9706-5516cb48c098";
        kimi.bearerHeader = true;

        OauthRefreshSpec openRouter;
        openRouter.providerId = "openrouter";
        openRouter.name = "OpenRouter OAuth";
        openRouter.isSubscription = false;
        openRouter.passthrough = true;

        return {anthropic, codex, chatgpt, xai, kimi, openRouter};
    }

    /** The Radius gateway's OAuth (client "pi-gateway"): `gateway` is the normalized origin, see RadiusGateway. */
    OauthRefreshSpec radius(const std::string& gateway) const {
        OauthRefreshSpec spec;
        spec.providerId = "radius";
        spec.name = "Radius";
        spec.isSubscription = false;
        spec.tokenUrl = gateway + "/v1/oauth/token";
        spec.clientId = "pi-gateway";
        spec.expiryMarginMs = 60 * 1000;
        spec.keepScope = true;
        return spec;
    }
};
