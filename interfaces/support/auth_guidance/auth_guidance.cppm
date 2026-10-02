export module pi.support.auth_guidance;

import std;

/**
 * User-facing texts for missing model or credentials. The backbone has no login UI, so these name
 * the problem and leave the remedy (selecting a model, logging in) to the client.
 */
export class AuthGuidance {
public:
    std::string noModelSelected() const;
    std::string noApiKeyFound(const std::string& provider) const;
    std::string authenticationFailed(const std::string& provider) const;
    std::string noModelsAvailable() const;
};

std::string AuthGuidance::noModelSelected() const {
    return "No model selected. Select a model before prompting.";
}

std::string AuthGuidance::noApiKeyFound(const std::string& provider) const {
    const std::string display = provider == "unknown" ? "the selected model" : provider;
    return "No API key found for " + display + ". Log in to the provider or configure an API key.";
}

std::string AuthGuidance::authenticationFailed(const std::string& provider) const {
    return "Authentication failed for \"" + provider +
           "\". Credentials may have expired or network is unavailable. Log in to " + provider +
           " again to re-authenticate.";
}

std::string AuthGuidance::noModelsAvailable() const {
    return "No models available. Log in to a provider or configure an API key.";
}
