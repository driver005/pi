module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.ai.remote_catalog_client;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_http_client;
export import pi.provider.i_models_store;
export import pi.support.header_merger;
export import pi.support.http_date_parser;

/**
 * Keeps the per-provider remote model catalog (default https://pi.dev) in the models store:
 * conditional GET with ETag, a four hour freshness window, tolerant parsing. The runtime merges
 * the stored overlay on its next reload. Port of core/remote-catalog-provider.ts.
 */
export class RemoteCatalogClient {
public:
    static constexpr std::int64_t RefreshIntervalMs = 4LL * 60 * 60 * 1000;

    RemoteCatalogClient(IHttpClient& http, IModelsStore& store, const IClock& clock,
                        std::string baseUrl = "https://pi.dev");

    /** True when the store changed. Error: transport failure or a non-2xx answer. */
    Result<bool> refresh(const std::string& providerId, bool force,
                         const std::shared_ptr<AbortSignal>& signal = nullptr);

private:
    Result<bool> store(const std::string& providerId, const ModelsStoreEntry& entry);
    Json parseModels(const std::string& providerId, const Json& body) const;
    HttpRequest buildRequest(const std::string& providerId, const ModelsStoreEntry* stored) const;
    std::string encode(const std::string& text) const;

    IHttpClient& m_http;
    IModelsStore& m_store;
    const IClock& m_clock;
    std::string m_baseUrl;
    HeaderMerger m_headers;
    HttpDateParser m_dates;
};

RemoteCatalogClient::RemoteCatalogClient(IHttpClient& http, IModelsStore& store, const IClock& clock,
                                         std::string baseUrl)
    : m_http(http), m_store(store), m_clock(clock), m_baseUrl(std::move(baseUrl)) {}

std::string RemoteCatalogClient::encode(const std::string& text) const {
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            constexpr char digits[] = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 15]);
        }
    }
    return out;
}

Result<bool> RemoteCatalogClient::store(const std::string& providerId, const ModelsStoreEntry& entry) {
    auto written = m_store.write(providerId, entry);
    if (!written) {
        return std::unexpected(written.error());
    }
    return true;
}

HttpRequest RemoteCatalogClient::buildRequest(const std::string& providerId,
                                              const ModelsStoreEntry* stored) const {
    std::string base = m_baseUrl;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    HttpRequest request;
    request.method = "GET";
    request.url = base + "/api/models/providers/" + encode(providerId) + "?types=chat,image,classifier";
    request.headers = {{"accept", "application/json"}, {"User-Agent", "pi"}};
    if (stored != nullptr && stored->etag && !stored->models.empty()) {
        request.headers.emplace_back("if-none-match", *stored->etag);
    }
    request.timeout = std::chrono::seconds(4);
    return request;
}

Json RemoteCatalogClient::parseModels(const std::string& providerId, const Json& body) const {
    Json models = Json::array();
    const auto add = [&](const Json& entry) {
        if (!entry.is_object() || !entry.contains("id")) {
            return;
        }
        if (entry.contains("type")) {
            const Json& type = entry["type"];
            if (!type.is_string()) {
                return;
            }
            const std::string name = type.get<std::string>();
            if (name != "chat" && name != "image" && name != "classifier") {
                return;
            }
        }
        Json copy = entry;
        copy["provider"] = providerId;
        models.push_back(std::move(copy));
    };
    if (body.is_array()) {
        for (const auto& entry : body) {
            add(entry);
        }
    } else if (body.is_object() && body.contains("models") && body["models"].is_array()) {
        for (const auto& entry : body["models"]) {
            add(entry);
        }
    } else if (body.is_object()) {
        for (const auto& [key, entry] : body.items()) {
            add(entry);
        }
    }
    return models;
}

Result<bool> RemoteCatalogClient::refresh(const std::string& providerId, bool force,
                                          const std::shared_ptr<AbortSignal>& signal) {
    auto read = m_store.read(providerId);
    if (!read) {
        return std::unexpected(read.error());
    }
    const std::optional<ModelsStoreEntry> stored = *read;
    const std::int64_t now = m_clock.nowMs();
    if (!force && stored && stored->checkedAt && stored->lastModified &&
        now - *stored->checkedAt < RefreshIntervalMs) {
        return false;
    }
    HttpRequest request = buildRequest(providerId, stored ? &*stored : nullptr);
    request.signal = signal;
    auto response = m_http.send(request);
    if (!response) {
        return std::unexpected(response.error());
    }
    ModelsStoreEntry base = stored.value_or(ModelsStoreEntry{});
    if (response->status == 304 && stored) {
        base.checkedAt = now;
        return store(providerId, base);
    }
    if (response->status == 404 || response->status == 501) {
        base.checkedAt = now;
        base.lastModified = 0;
        base.etag.reset();
        return store(providerId, base);
    }
    if (response->status < 200 || response->status >= 300) {
        base.checkedAt = now;
        if (auto written = store(providerId, base); !written) {
            return std::unexpected(written.error());
        }
        return std::unexpected(Error{"catalog_http", "Model catalog request failed for " + providerId +
                                                          ": " + std::to_string(response->status)});
    }
    const Json body = Json::parse(response->body, nullptr, false);
    if (body.is_discarded() || (!body.is_array() && !body.is_object())) {
        return std::unexpected(Error{"catalog_parse", "Invalid model catalog for provider \"" + providerId + "\""});
    }
    ModelsStoreEntry entry;
    entry.models = parseModels(providerId, body);
    entry.checkedAt = now;
    const auto modified = m_headers.find(response->headers, "last-modified");
    entry.lastModified = modified ? m_dates.parseMs(*modified).value_or(0) : 0;
    entry.etag = m_headers.find(response->headers, "etag");
    return store(providerId, entry);
}
