export module pi.support.radius_catalog;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_http_client;
export import pi.provider.i_models_store;
export import pi.support.radius_gateway;

/**
 * The model catalog of a Radius gateway: `GET <origin>/v1/config` (with the bearer token when there is one) answers
 * `{baseUrl, models: [...]}`; the models that have every required field become `pi-messages` models of the `radius` provider
 * and are kept in the models store (without a `lastModified`, so the runtime always applies them over the generated
 * catalog). A failed request leaves the stored catalog alone. Port of loadRadiusGatewayConfig and the refresh of
 * packages/ai/src/providers/radius.ts.
 */
export class RadiusCatalog {
public:
    RadiusCatalog(IHttpClient& http, IModelsStore& store, const IClock& clock)
        : m_http(http),
          m_store(store),
          m_clock(clock) {}

    /** Fetches the gateway's configuration and stores the catalog. Error: transport failure, non-2xx answer or an invalid body. */
    Result<void> refresh(const std::string& gateway, const std::optional<std::string>& apiKey, const std::shared_ptr<AbortSignal>& signal = nullptr) {
        HttpRequest request;
        request.method = "GET";
        request.url = origin(gateway) + "/v1/config";
        request.headers = {{"accept", "application/json"}};
        if (apiKey && !apiKey->empty()) {
            request.headers.emplace_back("authorization", "Bearer " + *apiKey);
        }
        request.timeout = std::chrono::seconds(5);
        request.signal = signal;
        auto response = m_http.send(request);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(Error{"radius_config", "Could not load Radius config from " + gateway + ": " + std::to_string(response->status) + ": " + truncate(response->body)});
        }
        const std::optional<Json> models = sanitize(Json::parse(response->body, nullptr, false));
        if (!models) {
            return std::unexpected(Error{"radius_config", "Invalid Radius config from " + gateway});
        }
        ModelsStoreEntry entry;
        entry.models = *models;
        entry.checkedAt = m_clock.nowMs();
        return m_store.write(RadiusGateway().providerId(), entry);
    }

private:
    /** `scheme://host[:port]` of a URL: `new URL("/v1/config", gateway)` resolves against the origin only. */
    std::string origin(const std::string& url) const {
        const std::size_t scheme = url.find("://");
        const std::size_t path = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
        return path == std::string::npos ? url : url.substr(0, path);
    }

    std::string truncate(const std::string& body) const {
        const std::size_t first = body.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        const std::string trimmed = body.substr(first, body.find_last_not_of(" \t\r\n") - first + 1);
        return trimmed.size() > 512 ? trimmed.substr(0, 512) + "..." : trimmed;
    }

    /** The valid models of a config body as `pi-messages` models of the provider; nothing when the body is not a config. */
    std::optional<Json> sanitize(const Json& config) const {
        if (!config.is_object() || !config.contains("baseUrl") || !config["baseUrl"].is_string() || !config.contains("models") || !config["models"].is_array()) {
            return std::nullopt;
        }
        Json models = Json::array();
        for (const Json& model : config["models"]) {
            if (!valid(model)) {
                continue;
            }
            Json copy = model;
            copy["api"] = "pi-messages";
            copy["provider"] = RadiusGateway().providerId();
            copy["baseUrl"] = config["baseUrl"];
            models.push_back(std::move(copy));
        }
        return models;
    }

    bool valid(const Json& model) const {
        return model.is_object() && model.value("id", Json()).is_string() && model.value("name", Json()).is_string() &&
               model.value("reasoning", Json()).is_boolean() && model.value("input", Json()).is_array() &&
               model.value("cost", Json()).is_object() && model.value("contextWindow", Json()).is_number() &&
               model.value("maxTokens", Json()).is_number();
    }

    IHttpClient& m_http;
    IModelsStore& m_store;
    const IClock& m_clock;
};
