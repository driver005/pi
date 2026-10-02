module;

#include <nlohmann/json.hpp>

export module pi.session.settings_manager;

import std;
export import pi.platform.i_file_lock;
export import pi.platform.i_file_system;
export import pi.platform.i_id_generator;
export import pi.session.i_settings_manager;
export import pi.support.json_writer;
export import pi.support.settings_merger;

/**
 * ISettingsManager over two JSON files. Port of SettingsManager in core/settings-manager.ts:
 * migration of legacy fields, deep merge, per-field persistence under the file lock, load
 * errors that disable saving for the affected scope.
 */
export class SettingsManager : public ISettingsManager {
public:
    SettingsManager(std::string globalPath, std::string projectPath, bool projectTrusted,
                    IFileSystem& files, IFileLock& lock, IIdGenerator& ids);

    Json settings() const override;
    Json globalSettings() const override;
    Json projectSettings() const override;
    SettingsView view() const override;
    bool projectTrusted() const override;
    Result<void> setProjectTrusted(bool trusted) override;
    void reload() override;
    void applyOverrides(const Json& overrides) override;
    Result<void> setGlobal(const std::string& field, const Json& value) override;
    Result<void> setGlobalNested(const std::string& field, const std::string& key,
                                 const Json& value) override;
    Result<void> removeGlobal(const std::string& field) override;
    Result<void> setProject(const std::string& field, const Json& value) override;
    Result<void> removeProject(const std::string& field) override;
    std::vector<SettingsError> drainErrors() override;
    Result<std::string> getOrCreateDeviceId() override;

private:
    Result<Json> loadScope(const std::string& path);
    void loadAll();
    void remerge();
    void record(const std::string& scope, const std::string& path, const std::string& message);
    Result<void> persist(bool global);
    Result<void> change(bool global, const std::function<void(Json&)>& edit, const std::string& field,
                        const std::optional<std::string>& nestedKey);
    std::string pathOf(bool global) const;
    Json mergedWithFile(const std::string& current, const Json& snapshot,
                        const std::set<std::string>& fields,
                        const std::map<std::string, std::set<std::string>>& nested) const;

    std::string m_globalPath;
    std::string m_projectPath;
    bool m_projectTrusted;
    IFileSystem& m_files;
    IFileLock& m_lock;
    IIdGenerator& m_ids;
    SettingsMerger m_merger;
    JsonWriter m_writer;

    Json m_global = Json::object();
    Json m_project = Json::object();
    Json m_merged = Json::object();
    Json m_overrides = Json::object();
    bool m_globalLoadError = false;
    bool m_projectLoadError = false;
    std::set<std::string> m_globalModified;
    std::map<std::string, std::set<std::string>> m_globalNested;
    std::set<std::string> m_projectModified;
    std::map<std::string, std::set<std::string>> m_projectNested;
    std::vector<SettingsError> m_errors;
};

SettingsManager::SettingsManager(std::string globalPath, std::string projectPath, bool projectTrusted,
                                 IFileSystem& files, IFileLock& lock, IIdGenerator& ids)
    : m_globalPath(std::move(globalPath)),
      m_projectPath(std::move(projectPath)),
      m_projectTrusted(projectTrusted),
      m_files(files),
      m_lock(lock),
      m_ids(ids) {
    loadAll();
}

std::string SettingsManager::pathOf(bool global) const {
    return global ? m_globalPath : m_projectPath;
}

void SettingsManager::record(const std::string& scope, const std::string& path, const std::string& message) {
    m_errors.push_back(SettingsError{scope, path, message});
}

Result<Json> SettingsManager::loadScope(const std::string& path) {
    if (path.empty()) {
        return Json::object();
    }
    auto text = m_files.readFile(path);
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
    if (body.empty()) {
        return Json::object();
    }
    Json parsed = Json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected(Error{"settings_parse", "Failed to parse " + path});
    }
    return m_merger.migrate(std::move(parsed));
}

void SettingsManager::loadAll() {
    auto global = loadScope(m_globalPath);
    if (global) {
        m_global = std::move(*global);
        m_globalLoadError = false;
    } else {
        m_globalLoadError = true;
        record("global", m_globalPath, global.error().message);
    }
    if (m_projectTrusted) {
        auto project = loadScope(m_projectPath);
        if (project) {
            m_project = std::move(*project);
            m_projectLoadError = false;
        } else {
            m_projectLoadError = true;
            record("project", m_projectPath, project.error().message);
        }
    } else {
        m_project = Json::object();
        m_projectLoadError = false;
    }
    remerge();
}

void SettingsManager::remerge() {
    m_merged = m_merger.merge(m_global, m_project);
    if (!m_overrides.empty()) {
        m_merged = m_merger.merge(m_merged, m_overrides);
    }
}

Json SettingsManager::settings() const {
    return m_merged;
}

Json SettingsManager::globalSettings() const {
    return m_global;
}

Json SettingsManager::projectSettings() const {
    return m_project;
}

SettingsView SettingsManager::view() const {
    return SettingsView(m_merged);
}

bool SettingsManager::projectTrusted() const {
    return m_projectTrusted;
}

Result<void> SettingsManager::setProjectTrusted(bool trusted) {
    if (m_projectTrusted == trusted) {
        return {};
    }
    m_projectTrusted = trusted;
    m_projectModified.clear();
    m_projectNested.clear();
    if (!trusted) {
        m_project = Json::object();
        m_projectLoadError = false;
        remerge();
        return {};
    }
    auto project = loadScope(m_projectPath);
    if (project) {
        m_project = std::move(*project);
        m_projectLoadError = false;
    } else {
        m_projectLoadError = true;
        record("project", m_projectPath, project.error().message);
    }
    remerge();
    return {};
}

void SettingsManager::reload() {
    auto global = loadScope(m_globalPath);
    if (global) {
        m_global = std::move(*global);
        m_globalLoadError = false;
    } else {
        m_globalLoadError = true;
        record("global", m_globalPath, global.error().message);
    }
    m_globalModified.clear();
    m_globalNested.clear();
    m_projectModified.clear();
    m_projectNested.clear();
    if (m_projectTrusted) {
        auto project = loadScope(m_projectPath);
        if (project) {
            m_project = std::move(*project);
            m_projectLoadError = false;
        } else {
            m_projectLoadError = true;
            record("project", m_projectPath, project.error().message);
        }
    }
    remerge();
}

void SettingsManager::applyOverrides(const Json& overrides) {
    m_overrides = m_merger.merge(m_overrides, overrides);
    remerge();
}

Json SettingsManager::mergedWithFile(const std::string& current, const Json& snapshot,
                                     const std::set<std::string>& fields,
                                     const std::map<std::string, std::set<std::string>>& nested) const {
    Json file = Json::object();
    if (current.find_first_not_of(" \t\r\n") != std::string::npos) {
        std::string body = current;
        if (body.rfind("\xEF\xBB\xBF", 0) == 0) {
            body.erase(0, 3);
        }
        Json parsed = Json::parse(body, nullptr, false);
        if (parsed.is_object()) {
            file = m_merger.migrate(std::move(parsed));
        }
    }
    Json merged = file;
    for (const auto& field : fields) {
        const bool present = snapshot.contains(field);
        const auto nestedKeys = nested.find(field);
        if (present && nestedKeys != nested.end() && snapshot[field].is_object()) {
            Json base = file.contains(field) && file[field].is_object() ? file[field] : Json::object();
            for (const auto& key : nestedKeys->second) {
                if (snapshot[field].contains(key)) {
                    base[key] = snapshot[field][key];
                } else {
                    base.erase(key);
                }
            }
            merged[field] = std::move(base);
        } else if (present) {
            merged[field] = snapshot[field];
        } else {
            merged.erase(field);
        }
    }
    return merged;
}

Result<void> SettingsManager::persist(bool global) {
    const std::string path = pathOf(global);
    if (path.empty()) {
        return {};
    }
    const Json snapshot = global ? m_global : m_project;
    const auto fields = global ? m_globalModified : m_projectModified;
    const auto nested = global ? m_globalNested : m_projectNested;
    const auto slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash == 0 ? 1 : slash);
    if (auto created = m_files.createDirectories(dir); !created) {
        return created;
    }
    Result<void> result;
    auto outcome = m_lock.withLock(path, [&]() -> Result<void> {
        auto current = m_files.readFile(path);
        const std::string text = current ? *current : std::string();
        result = m_files.writeFile(path, m_writer.pretty(mergedWithFile(text, snapshot, fields, nested), 2));
        return {};
    });
    if (!outcome) {
        return outcome;
    }
    if (result) {
        (global ? m_globalModified : m_projectModified).clear();
        (global ? m_globalNested : m_projectNested).clear();
    }
    return result;
}

Result<void> SettingsManager::change(bool global, const std::function<void(Json&)>& edit,
                                     const std::string& field, const std::optional<std::string>& nestedKey) {
    if (!global && !m_projectTrusted) {
        return std::unexpected(Error{"untrusted", "Project is not trusted; refusing to write project settings"});
    }
    Json& layer = global ? m_global : m_project;
    edit(layer);
    (global ? m_globalModified : m_projectModified).insert(field);
    if (nestedKey) {
        (global ? m_globalNested : m_projectNested)[field].insert(*nestedKey);
    }
    remerge();
    if (global ? m_globalLoadError : m_projectLoadError) {
        return {};
    }
    auto saved = persist(global);
    if (!saved) {
        record(global ? "global" : "project", pathOf(global), saved.error().message);
    }
    return saved;
}

Result<void> SettingsManager::setGlobal(const std::string& field, const Json& value) {
    return change(true, [&](Json& layer) { layer[field] = value; }, field, std::nullopt);
}

Result<void> SettingsManager::setGlobalNested(const std::string& field, const std::string& key,
                                              const Json& value) {
    return change(true,
                  [&](Json& layer) {
                      if (!layer.contains(field) || !layer[field].is_object()) {
                          layer[field] = Json::object();
                      }
                      layer[field][key] = value;
                  },
                  field, key);
}

Result<void> SettingsManager::removeGlobal(const std::string& field) {
    return change(true, [&](Json& layer) { layer.erase(field); }, field, std::nullopt);
}

Result<void> SettingsManager::setProject(const std::string& field, const Json& value) {
    return change(false, [&](Json& layer) { layer[field] = value; }, field, std::nullopt);
}

Result<void> SettingsManager::removeProject(const std::string& field) {
    return change(false, [&](Json& layer) { layer.erase(field); }, field, std::nullopt);
}

std::vector<SettingsError> SettingsManager::drainErrors() {
    std::vector<SettingsError> drained = std::move(m_errors);
    m_errors.clear();
    return drained;
}

Result<std::string> SettingsManager::getOrCreateDeviceId() {
    if (m_global.contains("deviceId") && m_global["deviceId"].is_string() &&
        !m_global["deviceId"].get<std::string>().empty()) {
        return m_global["deviceId"].get<std::string>();
    }
    const std::string id = m_ids.next();
    if (auto saved = setGlobal("deviceId", id); !saved) {
        return std::unexpected(saved.error());
    }
    return id;
}
