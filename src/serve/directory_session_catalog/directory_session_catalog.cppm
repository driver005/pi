export module pi.serve.directory_session_catalog;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_file_system;
export import pi.platform.i_id_generator;
export import pi.server.i_session_catalog;
import pi.types.json;

/**
 * ISessionCatalog over a directory: every session is a subdirectory named by its id holding
 * `meta.json` (`{createdAt, cwd}`, the format of the TS server) next to the session's own files.
 * Subdirectories without valid metadata are not sessions. Port of experimental/session-catalog.ts.
 */
export class DirectorySessionCatalog : public ISessionCatalog {
public:
    DirectorySessionCatalog(IFileSystem& files, const IClock& clock, IIdGenerator& ids, std::string root,
                            std::string cwd);

    Result<std::vector<SessionRecord>> list() override;
    Result<SessionRecord> create(const std::optional<std::string>& id) override;
    Result<void> remove(const std::string& id) override;
    Result<SessionRecord> resolve(const std::string& idOrPrefix) override;

private:
    bool validId(const std::string& id) const;
    std::optional<SessionRecord> read(const std::string& id);
    Error notFound() const;

    IFileSystem& m_files;
    const IClock& m_clock;
    IIdGenerator& m_ids;
    std::string m_root;
    std::string m_cwd;
    std::mutex m_mutex;
};

DirectorySessionCatalog::DirectorySessionCatalog(IFileSystem& files, const IClock& clock, IIdGenerator& ids,
                                                 std::string root, std::string cwd)
    : m_files(files), m_clock(clock), m_ids(ids), m_root(std::move(root)), m_cwd(std::move(cwd)) {}

Error DirectorySessionCatalog::notFound() const {
    return Error{"session_not_found", "Session was not found"};
}

bool DirectorySessionCatalog::validId(const std::string& id) const {
    if (id.empty() || id.size() > 128 || !std::isalnum(static_cast<unsigned char>(id.front()))) {
        return false;
    }
    return std::ranges::all_of(id, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
    });
}

std::optional<SessionRecord> DirectorySessionCatalog::read(const std::string& id) {
    if (!validId(id)) {
        return std::nullopt;
    }
    const std::string directory = m_root + "/" + id;
    auto content = m_files.readFile(directory + "/meta.json");
    if (!content) {
        return std::nullopt;
    }
    const Json meta = Json::parse(*content, nullptr, false);
    if (!meta.is_object() || !meta.value("createdAt", Json()).is_number() || !meta.value("cwd", Json()).is_string()) {
        return std::nullopt;
    }
    return SessionRecord{id, meta["createdAt"].get<std::int64_t>(), meta["cwd"].get<std::string>(), directory};
}

Result<std::vector<SessionRecord>> DirectorySessionCatalog::list() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SessionRecord> records;
    auto names = m_files.listDirectory(m_root);
    if (!names) {
        if (names.error().code == "ENOENT") {
            return records;
        }
        return std::unexpected(names.error());
    }
    for (const std::string& name : *names) {
        if (auto record = read(name)) {
            records.push_back(std::move(*record));
        }
    }
    std::ranges::sort(records, [](const SessionRecord& left, const SessionRecord& right) {
        return left.createdAt != right.createdAt ? left.createdAt > right.createdAt : left.id < right.id;
    });
    return records;
}

Result<SessionRecord> DirectorySessionCatalog::create(const std::optional<std::string>& id) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const std::string chosen = id ? *id : m_ids.next();
    if (!validId(chosen)) {
        return std::unexpected(Error{"invalid_request", "Invalid session ID: " + chosen});
    }
    const std::string directory = m_root + "/" + chosen;
    if (m_files.exists(directory)) {
        return std::unexpected(Error{"session_exists", "Session " + chosen + " already exists"});
    }
    if (auto made = m_files.createDirectories(directory); !made) {
        return std::unexpected(made.error());
    }
    SessionRecord record{chosen, m_clock.nowMs(), m_cwd, directory};
    const Json meta = {{"createdAt", record.createdAt}, {"cwd", record.cwd}};
    if (auto written = m_files.writeFile(directory + "/meta.json", meta.dump(1, '\t') + "\n"); !written) {
        m_files.removeTree(directory);
        return std::unexpected(written.error());
    }
    return record;
}

Result<void> DirectorySessionCatalog::remove(const std::string& id) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto record = read(id);
    if (!record) {
        return std::unexpected(notFound());
    }
    return m_files.removeTree(record->directory);
}

Result<SessionRecord> DirectorySessionCatalog::resolve(const std::string& idOrPrefix) {
    if (auto exact = [&] {
            const std::lock_guard<std::mutex> lock(m_mutex);
            return read(idOrPrefix);
        }()) {
        return *exact;
    }
    auto records = list();
    if (!records) {
        return std::unexpected(records.error());
    }
    std::vector<SessionRecord> matches;
    for (const SessionRecord& record : *records) {
        if (!idOrPrefix.empty() && record.id.starts_with(idOrPrefix)) {
            matches.push_back(record);
        }
    }
    if (matches.size() == 1) {
        return matches.front();
    }
    if (matches.size() > 1) {
        return std::unexpected(Error{"session_ambiguous", "Session ID matches more than one session"});
    }
    return std::unexpected(notFound());
}
