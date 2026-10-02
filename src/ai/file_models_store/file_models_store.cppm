module;

#include <nlohmann/json.hpp>

export module pi.ai.file_models_store;

import std;
export import pi.platform.i_file_lock;
export import pi.platform.i_file_system;
export import pi.provider.i_models_store;
export import pi.support.json_writer;

/** IModelsStore over models-store.json, format-compatible with the TypeScript FileModelsStore. */
export class FileModelsStore : public IModelsStore {
public:
    FileModelsStore(std::string path, IFileSystem& files, IFileLock& lock);

    Result<std::optional<ModelsStoreEntry>> read(const std::string& providerId) override;
    Result<void> write(const std::string& providerId, const ModelsStoreEntry& entry) override;
    Result<void> remove(const std::string& providerId) override;

private:
    Result<Json> load();
    Result<void> update(const std::function<void(Json&)>& change);
    Json toJson(const ModelsStoreEntry& entry) const;
    ModelsStoreEntry fromJson(const Json& json) const;
    std::string parentDirectory() const;

    std::string m_path;
    IFileSystem& m_files;
    IFileLock& m_lock;
    JsonWriter m_writer;
};

FileModelsStore::FileModelsStore(std::string path, IFileSystem& files, IFileLock& lock)
    : m_path(std::move(path)), m_files(files), m_lock(lock) {}

std::string FileModelsStore::parentDirectory() const {
    const auto slash = m_path.find_last_of('/');
    return slash == std::string::npos ? "." : m_path.substr(0, slash == 0 ? 1 : slash);
}

Json FileModelsStore::toJson(const ModelsStoreEntry& entry) const {
    Json out = Json::object();
    out["models"] = entry.models;
    if (entry.lastModified) {
        out["lastModified"] = *entry.lastModified;
    }
    if (entry.checkedAt) {
        out["checkedAt"] = *entry.checkedAt;
    }
    if (entry.etag) {
        out["etag"] = *entry.etag;
    }
    return out;
}

ModelsStoreEntry FileModelsStore::fromJson(const Json& json) const {
    ModelsStoreEntry entry;
    if (json.is_object()) {
        if (json.contains("models") && json["models"].is_array()) {
            entry.models = json["models"];
        }
        if (json.contains("lastModified") && json["lastModified"].is_number()) {
            entry.lastModified = json["lastModified"].get<std::int64_t>();
        }
        if (json.contains("checkedAt") && json["checkedAt"].is_number()) {
            entry.checkedAt = json["checkedAt"].get<std::int64_t>();
        }
        if (json.contains("etag") && json["etag"].is_string()) {
            entry.etag = json["etag"].get<std::string>();
        }
    }
    return entry;
}

Result<Json> FileModelsStore::load() {
    auto text = m_files.readFile(m_path);
    if (!text) {
        if (text.error().code == "ENOENT") {
            return Json::object();
        }
        return std::unexpected(text.error());
    }
    std::string body = *text;
    if (body.rfind("\xEF\xBB\xBF", 0) == 0) {
        body.erase(0, 3);
    }
    if (body.find_first_not_of(" \t\r\n") == std::string::npos) {
        return Json::object();
    }
    Json json = Json::parse(body, nullptr, false);
    if (!json.is_object()) {
        return std::unexpected(Error{"models_store", "Invalid models store: " + m_path});
    }
    return json;
}

Result<std::optional<ModelsStoreEntry>> FileModelsStore::read(const std::string& providerId) {
    auto json = load();
    if (!json) {
        return std::unexpected(json.error());
    }
    if (!json->contains(providerId)) {
        return std::optional<ModelsStoreEntry>();
    }
    return std::optional<ModelsStoreEntry>(fromJson((*json)[providerId]));
}

Result<void> FileModelsStore::update(const std::function<void(Json&)>& change) {
    if (auto ready = m_files.createPrivateDirectories(parentDirectory()); !ready) {
        return ready;
    }
    if (!m_files.exists(m_path)) {
        if (auto created = m_files.writeFilePrivate(m_path, "{}"); !created) {
            return created;
        }
    }
    Result<void> result;
    auto outcome = m_lock.withLock(m_path, [&]() -> Result<void> {
        auto json = load();
        if (!json) {
            result = std::unexpected(json.error());
            return {};
        }
        change(*json);
        result = m_files.writeFilePrivate(m_path, m_writer.pretty(*json, 2));
        return {};
    });
    if (!outcome) {
        return outcome;
    }
    return result;
}

Result<void> FileModelsStore::write(const std::string& providerId, const ModelsStoreEntry& entry) {
    return update([&](Json& json) { json[providerId] = toJson(entry); });
}

Result<void> FileModelsStore::remove(const std::string& providerId) {
    return update([&](Json& json) { json.erase(providerId); });
}
