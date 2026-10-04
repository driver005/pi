module;

#include <cstdint>

export module pi.support.oauth_device_login;

import std;
export import pi.platform.i_http_client;
export import pi.support.oauth_device_poller;
export import pi.support.url_parser;
export import pi.types.device_authorization;
export import pi.types.device_login_spec;
export import pi.types.login_interaction;

/**
 * The device authorization grant (RFC 8628): asks the provider for a device code, shows the user code and verification URL
 * through the interaction, polls the token endpoint until the user approved and returns the token response. The provider
 * specific parts are in the DeviceLoginSpec; turning the response into a credential is the caller's. Port of the device flows of
 * xai.ts, kimi-coding.ts and meta.ts in packages/ai/src/auth/oauth.
 *
 * Errors: "oauth" with the provider's wording for what the server reported, plus the poller's codes.
 */
export class OauthDeviceLogin {
public:
    OauthDeviceLogin(IHttpClient& http, const OauthDevicePoller& poller)
        : m_http(http),
          m_poller(poller) {}

    Result<Json> login(const DeviceLoginSpec& spec, const LoginInteraction& interaction) {
        auto device = requestDevice(spec, interaction.signal);
        if (!device) {
            return std::unexpected(device.error());
        }
        if (interaction.deviceCode) {
            const bool complete = spec.preferCompleteUri && !device->completeUri.empty();
            interaction.deviceCode(device->userCode, complete ? device->completeUri : device->uri, device->intervalSeconds, device->expiresInSeconds);
        }
        return m_poller.poll(device->intervalSeconds, device->expiresInSeconds, spec.waitBeforeFirstPoll, interaction.signal,
                             [&] { return pollOnce(spec, *device, interaction.signal); });
    }

private:
    Result<DeviceAuthorization> requestDevice(const DeviceLoginSpec& spec, const std::shared_ptr<AbortSignal>& signal) {
        std::vector<std::pair<std::string, std::string>> fields{{"client_id", spec.clientId}};
        if (!spec.scope.empty()) {
            fields.emplace_back("scope", spec.scope);
        }
        fields.insert(fields.end(), spec.deviceParams.begin(), spec.deviceParams.end());
        const auto response = m_http.send(form(spec, spec.deviceUrl, fields, signal));
        if (!response) {
            return std::unexpected(Error{"oauth", spec.name + " device authorization request failed: " + response.error().message});
        }
        const Json body = Json::parse(response->body, nullptr, false);
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(Error{"oauth", spec.name + " device authorization failed with status " + std::to_string(response->status) + detail(body)});
        }
        if (!body.is_object()) {
            return std::unexpected(Error{"oauth", spec.name + " OAuth returned invalid JSON (HTTP " + std::to_string(response->status) + ")"});
        }
        DeviceAuthorization device;
        device.deviceCode = text(body, "device_code");
        device.userCode = text(body, "user_code");
        device.uri = text(body, "verification_uri");
        device.completeUri = text(body, "verification_uri_complete");
        const bool complete = spec.requireCompleteUri || spec.preferCompleteUri;
        if (device.deviceCode.empty() || device.userCode.empty() || (device.uri.empty() && (device.completeUri.empty() || !complete)) ||
            (spec.requireCompleteUri && device.completeUri.empty())) {
            return std::unexpected(Error{"oauth", "Invalid " + spec.name + " device authorization response: " + response->body});
        }
        for (const std::string* uri : {&device.uri, &device.completeUri}) {
            if (!uri->empty() && !trusted(*uri, spec.httpsOnly)) {
                return std::unexpected(Error{"oauth", "Untrusted verification URI in " + spec.name + " OAuth response"});
            }
        }
        if (device.uri.empty()) {
            device.uri = device.completeUri;
        }
        if (positive(body, "interval")) {
            device.intervalSeconds = static_cast<std::int64_t>(body["interval"].get<double>());
        } else {
            device.intervalSeconds = spec.defaultIntervalSeconds;
        }
        if (positive(body, "expires_in")) {
            device.expiresInSeconds = static_cast<std::int64_t>(body["expires_in"].get<double>());
        } else if (spec.defaultExpiresSeconds > 0) {
            device.expiresInSeconds = spec.defaultExpiresSeconds;
        } else {
            return std::unexpected(Error{"oauth", "Invalid " + spec.name + " OAuth response field: expires_in"});
        }
        return device;
    }

    DevicePollResult pollOnce(const DeviceLoginSpec& spec, const DeviceAuthorization& device, const std::shared_ptr<AbortSignal>& signal) {
        const auto response = m_http.send(form(spec, spec.tokenUrl,
                                               {{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}, {"client_id", spec.clientId}, {spec.deviceCodeField, device.deviceCode}}, signal));
        if (!response) {
            return failed(spec.name + " device token request failed: " + response.error().message);
        }
        if (response->status >= 500 && spec.serverErrorFails) {
            return failed(spec.name + " device token request failed with status " + std::to_string(response->status) + (response->body.empty() ? "" : ": " + response->body));
        }
        const Json body = Json::parse(response->body, nullptr, false);
        if (response->status >= 200 && response->status < 300 && body.is_object() && !text(body, "access_token").empty()) {
            DevicePollResult done;
            done.status = "complete";
            done.value = body;
            return done;
        }
        const std::string error = text(body, "error");
        if (error == "authorization_pending") {
            return DevicePollResult{};
        }
        if (error == "slow_down") {
            DevicePollResult slow;
            slow.status = "slow_down";
            if (positive(body, "interval")) {
                slow.intervalSeconds = static_cast<std::int64_t>(body["interval"].get<double>());
            }
            return slow;
        }
        if (error == "access_denied" || error == "authorization_denied") {
            return failed(spec.name + " login was denied.");
        }
        if (error == "expired_token") {
            return failed(spec.name + " device authorization expired. Please restart login.");
        }
        return failed(spec.name + " device token request failed (status " + std::to_string(response->status) + ")" + (error.empty() ? "" : ": " + error + (text(body, "error_description").empty() ? "" : ": " + text(body, "error_description"))));
    }

    HttpRequest form(const DeviceLoginSpec& spec, const std::string& url, const std::vector<std::pair<std::string, std::string>>& fields, const std::shared_ptr<AbortSignal>& signal) const {
        HttpRequest request;
        request.method = "POST";
        request.url = url;
        request.headers = {{"Accept", "application/json"}, {"Content-Type", "application/x-www-form-urlencoded"}};
        request.headers.insert(request.headers.end(), spec.headers.begin(), spec.headers.end());
        for (const auto& [name, value] : fields) {
            request.body += (request.body.empty() ? "" : "&") + m_urls.encode(name) + "=" + m_urls.encode(value);
        }
        request.timeout = std::chrono::seconds(30);
        request.signal = signal;
        return request;
    }

    DevicePollResult failed(const std::string& message) const {
        DevicePollResult result;
        result.status = "failed";
        result.message = message;
        return result;
    }

    std::string text(const Json& object, const std::string& key) const {
        return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : "";
    }

    bool positive(const Json& object, const std::string& key) const {
        return object.is_object() && object.contains(key) && object[key].is_number() && object[key].get<double>() > 0;
    }

    bool trusted(const std::string& uri, bool httpsOnly) const {
        const auto parsed = m_urls.parse(uri);
        return parsed && (parsed->scheme == "https" || (!httpsOnly && parsed->scheme == "http"));
    }

    std::string detail(const Json& body) const {
        for (const char* key : {"error_description", "detail", "message", "error"}) {
            if (!text(body, key).empty()) {
                return ": " + text(body, key);
            }
        }
        return "";
    }

    IHttpClient& m_http;
    const OauthDevicePoller& m_poller;
    UrlParser m_urls;
};
