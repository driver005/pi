export module pi.types.credential;

import std;
export import pi.types.json;

export enum class CredentialType { ApiKey, OAuth };

/**
 * One stored credential per provider, the shape of an auth.json entry. API keys carry `key`
 * (literal, "$ENV" or "!command") and optional provider-scoped `env`; OAuth entries carry
 * access/refresh tokens and an expiry in epoch milliseconds. Unknown fields live in `extra`
 * so they survive a read-modify-write.
 */
export struct Credential {
    CredentialType type = CredentialType::ApiKey;
    std::optional<std::string> key;
    std::optional<std::map<std::string, std::string>> env;
    std::string access;
    std::string refresh;
    double expires = 0;
    Json extra = Json::object();
};
