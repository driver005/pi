module;

#include <cstdint>

export module pi.support.model_catalog_loader;

import std;
export import pi.platform.i_file_system;
export import pi.support.model_codec;
export import pi.types.model;
export import pi.types.result;

/**
 * Reads the generated model catalog written by the TypeScript `generate-models` script. Either
 * `<dir>/models.json` ({"<provider>": {"<model id>": model}}) or per-provider files
 * `<dir>/providers/<id>.json` ({"<api>": {"chat:<id>": model}}). Non-chat entries are skipped.
 */
export class ModelCatalogLoader {
public:
    explicit ModelCatalogLoader(IFileSystem& files)
        : m_files(files) {}

    /** provider id -> chat models, in file order. Missing directory: empty result. */
    Result<std::vector<std::pair<std::string, std::vector<Model>>>> load(const std::string& dir) {
        if (m_files.exists(dir + "/models.json")) {
            return loadFlat(dir + "/models.json");
        }
        return loadPerProvider(dir);
    }

    /** Modification time of the catalog, epoch milliseconds (0 when absent). */
    std::int64_t generatedAtMs(const std::string& dir) {
        if (auto info = m_files.stat(dir + "/models.json")) {
            return info->mtimeMs;
        }
        return 0;
    }

    /** Models from a raw array/object of model JSON for one provider; invalid entries skipped. */
    std::vector<Model> parseModels(const std::string& providerId, const Json& entries) const {
        std::vector<Model> models;
        const auto add = [&](const Json& entry) {
            if (!entry.is_object() || !entry.contains("id")) {
                return;
            }
            Json copy = entry;
            copy["provider"] = providerId;
            if (auto model = m_codec.fromJson(copy)) {
                models.push_back(std::move(*model));
            }
        };
        if (entries.is_array()) {
            for (const auto& entry : entries) {
                add(entry);
            }
        } else if (entries.is_object()) {
            for (const auto& item : entries.items()) {
                add(item.value());
            }
        }
        return models;
    }

private:
    using Catalog = std::vector<std::pair<std::string, std::vector<Model>>>;

    Result<Catalog> loadFlat(const std::string& path) {
        auto json = readJson(path);
        if (!json) {
            return std::unexpected(json.error());
        }
        Catalog catalog;
        if (!json->is_object()) {
            return std::unexpected(Error{"catalog", "Invalid model catalog: " + path});
        }
        for (const auto& entry : json->items()) {
            const std::string& providerId = entry.key();
            const Json& models = entry.value();
            catalog.emplace_back(providerId, parseModels(providerId, models));
        }
        return catalog;
    }

    Result<Catalog> loadPerProvider(const std::string& dir) {
        auto names = m_files.listDirectory(dir + "/providers");
        Catalog catalog;
        if (!names) {
            return catalog;
        }
        std::sort(names->begin(), names->end());
        for (const auto& name : *names) {
            if (name.size() <= 5 || name.substr(name.size() - 5) != ".json") {
                continue;
            }
            const std::string providerId = name.substr(0, name.size() - 5);
            auto json = readJson(dir + "/providers/" + name);
            if (!json) {
                return std::unexpected(json.error());
            }
            std::vector<Model> models;
            if (json->is_object()) {
                for (const auto& entry : json->items()) {
                    const Json& entries = entry.value();
                    for (auto& model : parseModels(providerId, entries)) {
                        models.push_back(std::move(model));
                    }
                }
            }
            catalog.emplace_back(providerId, std::move(models));
        }
        return catalog;
    }

    Result<Json> readJson(const std::string& path) {
        auto text = m_files.readFile(path);
        if (!text) {
            return std::unexpected(text.error());
        }
        const Json json = Json::parse(*text, nullptr, false);
        if (json.is_discarded()) {
            return std::unexpected(Error{"catalog", "Invalid model catalog: " + path});
        }
        return json;
    }

    IFileSystem& m_files;
    ModelCodec m_codec;
};
