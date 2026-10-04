export module pi.support.oauth_token_mapper;

import std;
export import pi.platform.i_base64_codec;
export import pi.platform.i_clock;
export import pi.platform.i_http_client;
export import pi.types.credential;
export import pi.types.json;
export import pi.types.oauth_refresh_spec;
export import pi.types.result;

/**
 * How one provider's token endpoint is spoken to and how its responses become credentials, described by an OauthRefreshSpec:
 * the refresh request, the form encoding and the mapping of a token response (access and refresh token, expiry, client id,
 * required scope, ChatGPT account id) onto a stored credential. Shared by the refresh of stored credentials and the sign-in
 * flows, so both store the same credential.
 */
export class OauthTokenMapper {
public:
    OauthTokenMapper(OauthRefreshSpec spec, const IClock& clock, const IBase64Codec& base64)
        : m_spec(std::move(spec)),
          m_clock(clock),
          m_base64(base64) {}

    const OauthRefreshSpec& spec() const {
        return m_spec;
    }

    Result<std::string> clientIdFor(const Credential& credential) const {
        if (!m_spec.clientId.empty()) {
            return m_spec.clientId;
        }
        const std::string stored = stringIn(credential.extra, "clientId");
        if (stored.find_first_not_of(" \t") == std::string::npos) {
            return std::unexpected(Error{"oauth", m_spec.missingClientIdMessage.empty()
                                                      ? "Stored OAuth credential does not contain a client ID"
                                                      : m_spec.missingClientIdMessage});
        }
        return stored;
    }

    HttpRequest refreshRequest(const Credential& credential, const std::string& clientId, const std::shared_ptr<AbortSignal>& signal) const {
        return tokenRequest({{"grant_type", "refresh_token"}, {"client_id", clientId}, {"refresh_token", credential.refresh}}, signal);
    }

    Result<Credential> credentialFrom(const Json& body, const Credential& previous, const std::string& clientId) const {
        const std::string access = stringIn(body, "access_token");
        if (access.find_first_not_of(" \t") == std::string::npos) {
            return std::unexpected(Error{"oauth", m_spec.name + " token response has invalid access_token"});
        }
        std::string refresh = stringIn(body, "refresh_token");
        if (refresh.empty()) {
            if (!m_spec.keepRefreshWhenMissing) {
                return std::unexpected(Error{"oauth", m_spec.name + " token response has invalid refresh_token"});
            }
            refresh = previous.refresh;
        }
        double lifetime = 3600;
        if (body.contains("expires_in")) {
            if (!body["expires_in"].is_number() || body["expires_in"].get<double>() <= 0) {
                return std::unexpected(Error{"oauth", m_spec.name + " token response has invalid expires_in"});
            }
            lifetime = body["expires_in"].get<double>();
        }
        Credential out;
        out.type = CredentialType::OAuth;
        out.access = access;
        out.refresh = refresh;
        out.expires = static_cast<double>(m_clock.nowMs()) + lifetime * 1000.0 - static_cast<double>(m_spec.expiryMarginMs);
        out.extra = previous.extra.is_object() ? previous.extra : Json::object();
        if (m_spec.clientId.empty()) {
            out.extra["clientId"] = clientId;
        }
        if (!m_spec.requiredScope.empty()) {
            const std::string scope = stringIn(body, "scope");
            Json scopes = Json::array();
            std::istringstream words(scope);
            for (std::string word; words >> word;) {
                scopes.push_back(word);
            }
            if (std::find(scopes.begin(), scopes.end(), Json(m_spec.requiredScope)) == scopes.end()) {
                return std::unexpected(Error{"oauth", m_spec.name + " grant did not include " + m_spec.requiredScope});
            }
            out.extra["scopes"] = std::move(scopes);
        }
        if (m_spec.keepScope && !stringIn(body, "scope").empty()) {
            out.extra["scope"] = stringIn(body, "scope");
        }
        if (m_spec.accountIdFromJwt) {
            const auto id = accountId(access);
            if (!id) {
                return std::unexpected(Error{"oauth", "Failed to extract accountId from token"});
            }
            out.extra["accountId"] = *id;
        }
        return out;
    }

    std::string formEncode(const std::string& text) const {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string out;
        for (const char c : text) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
                out.push_back(c);
            } else {
                out.push_back('%');
                out.push_back(digits[(static_cast<unsigned char>(c) >> 4) & 0xF]);
                out.push_back(digits[static_cast<unsigned char>(c) & 0xF]);
            }
        }
        return out;
    }

    std::optional<std::string> accountId(const std::string& token) const {
        const auto first = token.find('.');
        const auto second = first == std::string::npos ? std::string::npos : token.find('.', first + 1);
        if (second == std::string::npos) {
            return std::nullopt;
        }
        const auto payload = m_base64.decode(token.substr(first + 1, second - first - 1));
        const Json claims = payload ? Json::parse(*payload, nullptr, false) : Json();
        if (claims.is_object() && claims.contains("https://api.openai.com/auth") && claims["https://api.openai.com/auth"].is_object()) {
            const std::string id = stringIn(claims["https://api.openai.com/auth"], "chatgpt_account_id");
            return id.empty() ? std::nullopt : std::optional<std::string>(id);
        }
        return std::nullopt;
    }

    std::string stringIn(const Json& object, const std::string& key) const {
        return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : "";
    }

    /** The request that exchanges `params` at the token endpoint (grant, client id and the spec's extras are the caller's). */
    HttpRequest tokenRequest(std::map<std::string, std::string> params, const std::shared_ptr<AbortSignal>& signal) const {
        for (const auto& extra : m_spec.extraParams) {
            params.emplace(extra.first, extra.second);
        }
        HttpRequest request;
        request.method = "POST";
        request.url = m_spec.tokenUrl;
        request.signal = signal;
        request.timeout = std::chrono::seconds(30);
        request.headers = {{"Accept", "application/json"}};
        if (m_spec.jsonBody) {
            request.headers.emplace_back("Content-Type", "application/json");
            request.body = Json(params).dump();
        } else {
            request.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
            for (const auto& entry : params) {
                request.body += (request.body.empty() ? "" : "&") + formEncode(entry.first) + "=" + formEncode(entry.second);
            }
        }
        return request;
    }

private:
    OauthRefreshSpec m_spec;
    const IClock& m_clock;
    const IBase64Codec& m_base64;
};
