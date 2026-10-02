module;

#include <nlohmann/json.hpp>

export module pi.support.credential_codec;

import std;
export import pi.types.credential;
export import pi.types.json;
export import pi.types.result;

/** auth.json entry <-> Credential. Validation mirrors the TypeScript ReadOnlyAuthStorage. */
export class CredentialCodec {
public:
    Json toJson(const Credential& credential) const;
    Result<Credential> fromJson(const std::string& providerId, const Json& json) const;

    /** A whole auth.json document: object of provider id -> credential. */
    Result<std::vector<std::pair<std::string, Credential>>> parseDocument(
        const std::string& text) const;
    std::string serializeDocument(
        const std::vector<std::pair<std::string, Credential>>& entries) const;
};

Json CredentialCodec::toJson(const Credential& credential) const {
    Json out = Json::object();
    if (credential.type == CredentialType::ApiKey) {
        out["type"] = "api_key";
        if (credential.key) {
            out["key"] = *credential.key;
        }
        if (credential.env) {
            Json env = Json::object();
            for (const auto& [name, value] : *credential.env) {
                env[name] = value;
            }
            out["env"] = std::move(env);
        }
    } else {
        out["type"] = "oauth";
        out["refresh"] = credential.refresh;
        out["access"] = credential.access;
        if (credential.expires == std::floor(credential.expires) &&
            std::abs(credential.expires) < 9e15) {
            out["expires"] = static_cast<std::int64_t>(credential.expires);
        } else {
            out["expires"] = credential.expires;
        }
    }
    if (credential.extra.is_object()) {
        for (const auto& [name, value] : credential.extra.items()) {
            out[name] = value;
        }
    }
    return out;
}

Result<Credential> CredentialCodec::fromJson(const std::string& providerId, const Json& json) const {
    const Error invalid{"invalid_credential",
                        "Invalid auth.json credential for provider \"" + providerId + "\""};
    if (!json.is_object() || !json.contains("type") || !json["type"].is_string()) {
        return std::unexpected(invalid);
    }
    Credential credential;
    const std::string type = json["type"].get<std::string>();
    std::set<std::string> known = {"type"};
    if (type == "api_key") {
        credential.type = CredentialType::ApiKey;
        known.insert({"key", "env"});
        if (json.contains("key")) {
            if (!json["key"].is_string()) {
                return std::unexpected(invalid);
            }
            credential.key = json["key"].get<std::string>();
        }
        if (json.contains("env")) {
            if (!json["env"].is_object()) {
                return std::unexpected(invalid);
            }
            std::map<std::string, std::string> env;
            for (const auto& [name, value] : json["env"].items()) {
                if (!value.is_string()) {
                    return std::unexpected(invalid);
                }
                env[name] = value.get<std::string>();
            }
            credential.env = std::move(env);
        }
    } else if (type == "oauth") {
        credential.type = CredentialType::OAuth;
        known.insert({"access", "refresh", "expires"});
        if (!json.contains("access") || !json["access"].is_string() || !json.contains("refresh") ||
            !json["refresh"].is_string() || !json.contains("expires") ||
            !json["expires"].is_number()) {
            return std::unexpected(invalid);
        }
        credential.access = json["access"].get<std::string>();
        credential.refresh = json["refresh"].get<std::string>();
        credential.expires = json["expires"].get<double>();
    } else {
        return std::unexpected(invalid);
    }
    for (const auto& [name, value] : json.items()) {
        if (!known.contains(name)) {
            credential.extra[name] = value;
        }
    }
    return credential;
}

Result<std::vector<std::pair<std::string, Credential>>> CredentialCodec::parseDocument(
    const std::string& text) const {
    std::string body = text;
    if (body.rfind("\xEF\xBB\xBF", 0) == 0) {
        body.erase(0, 3);
    }
    std::vector<std::pair<std::string, Credential>> entries;
    if (body.find_first_not_of(" \t\r\n") == std::string::npos) {
        return entries;
    }
    const Json json = Json::parse(body, nullptr, false);
    if (!json.is_object()) {
        return std::unexpected(Error{"invalid_auth_file", "Invalid auth.json: expected an object"});
    }
    for (const auto& [providerId, value] : json.items()) {
        auto credential = fromJson(providerId, value);
        if (!credential) {
            return std::unexpected(credential.error());
        }
        entries.emplace_back(providerId, std::move(*credential));
    }
    return entries;
}

std::string CredentialCodec::serializeDocument(
    const std::vector<std::pair<std::string, Credential>>& entries) const {
    Json document = Json::object();
    for (const auto& [providerId, credential] : entries) {
        document[providerId] = toJson(credential);
    }
    return document.dump(2, ' ', false, Json::error_handler_t::replace);
}
