export module pi.session.project_trust_store;

import std;
export import pi.platform.i_file_lock;
export import pi.platform.i_file_system;
export import pi.session.i_project_trust_store;
import pi.support.json_writer;

/**
 * IProjectTrustStore over <agentDir>/trust.json, compatible with the TypeScript store: an object
 * of canonical directory path to true/false/null, written with sorted keys under the
 * "<trust.json>.lock" lock. Lookups walk from the cwd up through its ancestors.
 */
export class ProjectTrustStore : public IProjectTrustStore {
public:
    ProjectTrustStore(const std::string& agentDir, IFileSystem& files, IFileLock& lock);

    Result<std::optional<bool>> get(const std::string& cwd) override;
    Result<std::optional<ProjectTrustEntry>> getEntry(const std::string& cwd) override;
    Result<void> set(const std::string& cwd, std::optional<bool> decision) override;
    Result<void> setMany(const std::vector<ProjectTrustUpdate>& updates) override;

private:
    using Decisions = std::map<std::string, std::optional<bool>>;

    Result<Decisions> load();
    Result<void> save(const Decisions& decisions);
    std::string canonical(const std::string& path);
    std::string parentOf(const std::string& path) const;

    std::string m_path;
    IFileSystem& m_files;
    IFileLock& m_lock;
    JsonWriter m_writer;
};

ProjectTrustStore::ProjectTrustStore(const std::string& agentDir, IFileSystem& files,
                                     IFileLock& lock)
    : m_path(agentDir + "/trust.json"), m_files(files), m_lock(lock) {}

std::string ProjectTrustStore::canonical(const std::string& path) {
    return m_files.realPath(path);
}

std::string ProjectTrustStore::parentOf(const std::string& path) const {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

Result<ProjectTrustStore::Decisions> ProjectTrustStore::load() {
    auto text = m_files.readFile(m_path);
    if (!text) {
        if (text.error().code == "ENOENT") {
            return Decisions{};
        }
        return std::unexpected(text.error());
    }
    std::string_view body = *text;
    if (body.starts_with("\xEF\xBB\xBF")) {
        body.remove_prefix(3);
    }
    const Json parsed = Json::parse(body, nullptr, false);
    if (parsed.is_discarded()) {
        return std::unexpected(
            Error{"invalid_json", "Failed to read trust store " + m_path + ": invalid JSON"});
    }
    if (!parsed.is_object()) {
        return std::unexpected(
            Error{"invalid_json", "Invalid trust store " + m_path + ": expected an object"});
    }
    Decisions decisions;
    for (const auto& entry : parsed.items()) {
        const std::string& key = entry.key();
        const Json& value = entry.value();
        if (value.is_null()) {
            decisions[key] = std::nullopt;
        } else if (value.is_boolean()) {
            decisions[key] = value.get<bool>();
        } else {
            return std::unexpected(Error{"invalid_json", "Invalid trust store " + m_path +
                                                             ": value for \"" + key +
                                                             "\" must be true, false, or null"});
        }
    }
    return decisions;
}

Result<void> ProjectTrustStore::save(const Decisions& decisions) {
    Json document = Json::object();
    for (const auto& [key, value] : decisions) {
        document[key] = value.has_value() ? Json(*value) : Json(nullptr);
    }
    return m_files.writeFile(m_path, m_writer.pretty(document) + "\n");
}

Result<std::optional<ProjectTrustEntry>> ProjectTrustStore::getEntry(const std::string& cwd) {
    if (auto created = m_files.createDirectories(parentOf(m_path)); !created) {
        return std::unexpected(created.error());
    }
    Result<std::optional<ProjectTrustEntry>> result = std::optional<ProjectTrustEntry>();
    auto outcome = m_lock.withLock(m_path, [&]() -> Result<void> {
        auto decisions = load();
        if (!decisions) {
            result = std::unexpected(decisions.error());
            return {};
        }
        std::string current = canonical(cwd);
        while (true) {
            const auto found = decisions->find(current);
            if (found != decisions->end() && found->second.has_value()) {
                result = std::optional<ProjectTrustEntry>(ProjectTrustEntry{current, *found->second});
                return {};
            }
            const std::string parent = parentOf(current);
            if (parent == current) {
                return {};
            }
            current = parent;
        }
    });
    if (!outcome) {
        return std::unexpected(outcome.error());
    }
    return result;
}

Result<std::optional<bool>> ProjectTrustStore::get(const std::string& cwd) {
    auto entry = getEntry(cwd);
    if (!entry) {
        return std::unexpected(entry.error());
    }
    if (!entry->has_value()) {
        return std::optional<bool>();
    }
    return std::optional<bool>((*entry)->decision);
}

Result<void> ProjectTrustStore::set(const std::string& cwd, std::optional<bool> decision) {
    return setMany({ProjectTrustUpdate{cwd, decision}});
}

Result<void> ProjectTrustStore::setMany(const std::vector<ProjectTrustUpdate>& updates) {
    if (auto created = m_files.createDirectories(parentOf(m_path)); !created) {
        return created;
    }
    Result<void> result;
    auto outcome = m_lock.withLock(m_path, [&]() -> Result<void> {
        auto decisions = load();
        if (!decisions) {
            result = std::unexpected(decisions.error());
            return {};
        }
        for (const auto& update : updates) {
            const std::string key = canonical(update.path);
            if (update.decision.has_value()) {
                (*decisions)[key] = update.decision;
            } else {
                decisions->erase(key);
            }
        }
        result = save(*decisions);
        return {};
    });
    if (!outcome) {
        return outcome;
    }
    return result;
}
