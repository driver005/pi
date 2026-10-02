module;

#include <nlohmann/json.hpp>

export module pi.ai.google_adc_auth;

import std;
export import pi.platform.i_base64_codec;
export import pi.platform.i_clock;
export import pi.platform.i_crypto;
export import pi.platform.i_environment;
export import pi.platform.i_file_system;
export import pi.platform.i_http_client;
export import pi.provider.i_access_token_source;
export import pi.types.json;

/**
 * Google Application Default Credentials: a service account key (signed JWT exchanged for a
 * token), an authorized-user refresh token, or the metadata server on Google Cloud. The key file
 * comes from GOOGLE_APPLICATION_CREDENTIALS, else gcloud's application_default_credentials.json
 * ($CLOUDSDK_CONFIG or ~/.config/gcloud). Tokens are cached until a minute before they expire.
 */
export class GoogleAdcAuth : public IAccessTokenSource {
public:
    GoogleAdcAuth(IHttpClient& http, IFileSystem& files, const IEnvironment& environment, const IClock& clock,
                  ICrypto& crypto, const IBase64Codec& base64);

    Result<std::string> token(const std::map<std::string, std::string>& env) override;

private:
    std::optional<std::string> envValue(const std::map<std::string, std::string>& env, const std::string& name) const;
    std::string credentialsPath(const std::map<std::string, std::string>& env) const;
    Result<std::string> fetch(const std::string& path);
    Result<std::string> fromServiceAccount(const Json& key);
    Result<std::string> fromAuthorizedUser(const Json& key);
    Result<std::string> fromMetadataServer();
    Result<std::string> signedAssertion(const Json& key, const std::string& audience) const;
    Result<std::string> exchange(const std::string& url, const std::string& form);
    Result<std::string> remember(const std::string& cacheKey, const HttpResponse& response);
    std::string formEncode(const std::string& text) const;

    IHttpClient& m_http;
    IFileSystem& m_files;
    const IEnvironment& m_environment;
    const IClock& m_clock;
    ICrypto& m_crypto;
    const IBase64Codec& m_base64;
    std::mutex m_mutex;
    std::string m_cachedFor;
    std::string m_cachedToken;
    std::int64_t m_cachedUntilMs = 0;
};

GoogleAdcAuth::GoogleAdcAuth(IHttpClient& http, IFileSystem& files, const IEnvironment& environment,
                             const IClock& clock, ICrypto& crypto, const IBase64Codec& base64)
    : m_http(http), m_files(files), m_environment(environment), m_clock(clock), m_crypto(crypto), m_base64(base64) {}

std::optional<std::string> GoogleAdcAuth::envValue(const std::map<std::string, std::string>& env,
                                                   const std::string& name) const {
    const auto scoped = env.find(name);
    if (scoped != env.end() && !scoped->second.empty()) {
        return scoped->second;
    }
    const auto process = m_environment.get(name);
    return process && !process->empty() ? process : std::nullopt;
}

std::string GoogleAdcAuth::credentialsPath(const std::map<std::string, std::string>& env) const {
    if (const auto explicitPath = envValue(env, "GOOGLE_APPLICATION_CREDENTIALS")) {
        return *explicitPath;
    }
    const std::string config = envValue(env, "CLOUDSDK_CONFIG").value_or(m_files.homeDirectory() + "/.config/gcloud");
    return config + "/application_default_credentials.json";
}

std::string GoogleAdcAuth::formEncode(const std::string& text) const {
    std::string out;
    for (const char c : text) {
        const bool safe = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            out.push_back(c);
        } else {
            constexpr char digits[] = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(digits[(static_cast<unsigned char>(c) >> 4) & 0xF]);
            out.push_back(digits[static_cast<unsigned char>(c) & 0xF]);
        }
    }
    return out;
}

Result<std::string> GoogleAdcAuth::remember(const std::string& cacheKey, const HttpResponse& response) {
    if (response.status < 200 || response.status >= 300) {
        return std::unexpected(Error{"auth_failed", "Google token request failed (" + std::to_string(response.status) +
                                                        "): " + response.body.substr(0, 500)});
    }
    const Json body = Json::parse(response.body, nullptr, false);
    if (!body.is_object() || !body.contains("access_token") || !body["access_token"].is_string()) {
        return std::unexpected(Error{"auth_failed", "Google token response has no access_token"});
    }
    const std::int64_t lifetimeSeconds = body.contains("expires_in") && body["expires_in"].is_number()
                                             ? body["expires_in"].get<std::int64_t>()
                                             : 3600;
    m_cachedFor = cacheKey;
    m_cachedToken = body["access_token"].get<std::string>();
    m_cachedUntilMs = m_clock.nowMs() + (lifetimeSeconds - 60) * 1000;
    return m_cachedToken;
}

Result<std::string> GoogleAdcAuth::exchange(const std::string& url, const std::string& form) {
    HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
    request.body = form;
    request.timeout = std::chrono::seconds(30);
    const auto response = m_http.send(request);
    if (!response) {
        return std::unexpected(response.error());
    }
    return remember(m_cachedFor, *response);
}

Result<std::string> GoogleAdcAuth::signedAssertion(const Json& key, const std::string& audience) const {
    const std::int64_t issued = m_clock.nowMs() / 1000;
    const Json header = Json{{"alg", "RS256"}, {"typ", "JWT"}};
    const Json claims = Json{{"iss", key.value("client_email", "")},
                             {"scope", "https://www.googleapis.com/auth/cloud-platform"},
                             {"aud", audience},
                             {"iat", issued},
                             {"exp", issued + 3600}};
    const std::string unsignedToken = m_base64.encodeUrl(header.dump()) + "." + m_base64.encodeUrl(claims.dump());
    const auto signature = m_crypto.rsaSha256Sign(key.value("private_key", ""), unsignedToken);
    if (!signature) {
        return std::unexpected(Error{"auth_failed", "Could not sign the service account assertion: " +
                                                        signature.error().message});
    }
    return unsignedToken + "." + m_base64.encodeUrl(*signature);
}

Result<std::string> GoogleAdcAuth::fromServiceAccount(const Json& key) {
    const std::string tokenUri = key.value("token_uri", "https://oauth2.googleapis.com/token");
    const auto assertion = signedAssertion(key, tokenUri);
    if (!assertion) {
        return std::unexpected(assertion.error());
    }
    return exchange(tokenUri, "grant_type=" + formEncode("urn:ietf:params:oauth:grant-type:jwt-bearer") +
                                  "&assertion=" + formEncode(*assertion));
}

Result<std::string> GoogleAdcAuth::fromAuthorizedUser(const Json& key) {
    return exchange("https://oauth2.googleapis.com/token",
                    "grant_type=refresh_token&client_id=" + formEncode(key.value("client_id", "")) +
                        "&client_secret=" + formEncode(key.value("client_secret", "")) +
                        "&refresh_token=" + formEncode(key.value("refresh_token", "")));
}

Result<std::string> GoogleAdcAuth::fromMetadataServer() {
    HttpRequest request;
    request.url = "http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token";
    request.headers = {{"Metadata-Flavor", "Google"}};
    request.timeout = std::chrono::seconds(3);
    const auto response = m_http.send(request);
    if (!response) {
        return std::unexpected(Error{"auth_failed", "Could not load Google credentials: no credentials file and the "
                                                    "metadata server is unreachable"});
    }
    return remember("metadata", *response);
}

Result<std::string> GoogleAdcAuth::fetch(const std::string& path) {
    if (!m_files.exists(path)) {
        m_cachedFor = "metadata";
        return fromMetadataServer();
    }
    const auto text = m_files.readFile(path);
    if (!text) {
        return std::unexpected(Error{"auth_failed", "Could not read Google credentials " + path + ": " +
                                                        text.error().message});
    }
    const Json key = Json::parse(*text, nullptr, false);
    const std::string type = key.is_object() ? key.value("type", "") : "";
    m_cachedFor = path;
    if (type == "service_account") {
        return fromServiceAccount(key);
    }
    if (type == "authorized_user") {
        return fromAuthorizedUser(key);
    }
    return std::unexpected(Error{"auth_failed", "Unsupported Google credentials type in " + path});
}

Result<std::string> GoogleAdcAuth::token(const std::map<std::string, std::string>& env) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const std::string path = credentialsPath(env);
    if (!m_cachedToken.empty() && m_cachedUntilMs > m_clock.nowMs() &&
        (m_cachedFor == path || (m_cachedFor == "metadata" && !m_files.exists(path)))) {
        return m_cachedToken;
    }
    return fetch(path);
}
