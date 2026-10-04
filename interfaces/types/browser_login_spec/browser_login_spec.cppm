export module pi.types.browser_login_spec;

import std;

/**
 * The authorization code flow with PKCE (RFC 7636) of one provider: where the user is sent, where the browser comes back to
 * (a loopback address) and how the code is exchanged. The exchange itself is described by the provider's OauthRefreshSpec.
 */
export struct BrowserLoginSpec {
    std::string authorizeUrl;
    /** Fixed client id; empty takes the one the callback carries (dynamic registration, `dynamicClientId`). */
    std::string clientId;
    std::string scope;
    /** Host written into the redirect URI ("localhost" or "127.0.0.1"); the callback server listens on loopback. */
    std::string redirectHost = "localhost";
    /** Port of the callback server; 0 picks a free one. */
    int port = 0;
    std::string path = "/callback";
    /** Appends a random segment to `path`, so a stray request cannot complete the sign-in. */
    bool randomPath = false;
    /** The port is shared with other tools: when it is taken, the code is pasted instead (else the sign-in fails). */
    bool pasteWhenPortIsTaken = false;
    /** The PKCE verifier doubles as `state`; false uses a random state, `noState` none. */
    bool stateIsVerifier = false;
    bool noState = false;
    /** Fixed extra query parameters of the authorization URL. */
    std::vector<std::pair<std::string, std::string>> authorizeParams;
    /** Name of the redirect parameter ("redirect_uri"; OpenRouter calls it "callback_url"). */
    std::string redirectParam = "redirect_uri";
    /** Sends the client id and scope with the authorization request. */
    bool sendClientId = true;
    /** The token request carries `state` too (Anthropic). */
    bool stateInTokenRequest = false;
    /** The callback carries the client id the server issued (Sign in with ChatGPT). */
    bool dynamicClientId = false;
    /** The token response must carry an `id_token`. */
    bool requireIdToken = false;
    /** The code is exchanged for a long-lived API key as JSON `{code, code_verifier, code_challenge_method}` posted to `exchangeUrl` (OpenRouter). */
    bool exchangesForKey = false;
    std::string exchangeUrl;
    /** Redirect URI of the copy-code variant: the provider shows the code and the user pastes it. Empty: no such variant. */
    std::string copyCodeRedirectUri;
    /** What the success page calls the provider. */
    std::string providerName;
};
