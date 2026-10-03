module;

#include <cstdint>

export module pi.session.session_store;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_file_system;
export import pi.platform.i_id_generator;
export import pi.session.i_session_manager_factory;
export import pi.session.i_session_store;
export import pi.support.iso_timestamp;
export import pi.support.path_resolver;
export import pi.support.session_entry_codec;
export import pi.support.session_info_builder;

/**
 * ISessionStore over the agent directory's `sessions/` tree. Port of the static discovery
 * helpers of SessionManager in core/session-manager.ts.
 */
export class SessionStore : public ISessionStore {
public:
    static constexpr std::size_t MaxHeaderScanBytes = 1024 * 1024;

    SessionStore(std::string agentDir, IFileSystem& files, const IClock& clock, IIdGenerator& ids, ISessionManagerFactory& factory)
        : m_agentDir(std::move(agentDir)),
          m_files(files),
          m_clock(clock),
          m_ids(ids),
          m_factory(factory),
          m_paths(files.homeDirectory()) {}

    std::string defaultSessionDir(const std::string& cwd) override {
        const std::string dir = defaultDirPath(cwd);
        if (!m_files.exists(dir)) {
            m_files.createDirectories(dir);
        }
        return dir;
    }

    Result<std::unique_ptr<ISessionManager>> create(const std::string& cwd, const std::optional<std::string>& sessionDir, const std::optional<std::string>& id, const std::optional<std::string>& parentSession) override {
        SessionManagerOptions options;
        options.cwd = cwd;
        options.sessionDir = sessionDir ? resolve(*sessionDir) : defaultSessionDir(cwd);
        options.id = id;
        options.parentSession = parentSession;
        return m_factory.create(options);
    }

    Result<std::unique_ptr<ISessionManager>> open(const std::string& path, const std::optional<std::string>& sessionDir, const std::optional<std::string>& cwdOverride) override {
        const std::string resolved = resolve(path);
        std::string cwd;
        if (cwdOverride) {
            cwd = *cwdOverride;
        } else if (const auto header = m_files.exists(resolved) ? readHeader(resolved) : std::nullopt;
                   header && !header->cwd.empty()) {
            cwd = header->cwd;
        } else {
            cwd = m_files.homeDirectory();
        }
        SessionManagerOptions options;
        options.cwd = cwd;
        options.sessionDir = sessionDir ? resolve(*sessionDir) : directoryOf(resolved);
        options.sessionFile = resolved;
        return m_factory.create(options);
    }

    Result<std::unique_ptr<ISessionManager>> continueRecent(const std::string& cwd, const std::optional<std::string>& sessionDir) override {
        const std::string dir = sessionDir ? resolve(*sessionDir) : defaultSessionDir(cwd);
        const bool filterCwd = sessionDir && dir != defaultDirPath(cwd);
        const auto recent = findMostRecent(dir, filterCwd ? std::optional<std::string>(cwd) : std::nullopt);
        SessionManagerOptions options;
        options.cwd = cwd;
        options.sessionDir = dir;
        options.sessionFile = recent;
        return m_factory.create(options);
    }

    Result<std::unique_ptr<ISessionManager>> inMemory(const std::string& cwd) override {
        SessionManagerOptions options;
        options.cwd = cwd;
        options.persist = false;
        return m_factory.create(options);
    }

    Result<std::unique_ptr<ISessionManager>> forkFrom(const std::string& sourcePath, const std::string& targetCwd, const std::optional<std::string>& sessionDir, const std::optional<std::string>& id) override {
        const std::string source = resolve(sourcePath);
        const std::string cwd = resolve(targetCwd);
        auto content = m_files.readFile(source);
        if (!content) {
            return std::unexpected(Error{"fork_failed", "Cannot fork: source session file is empty or invalid: " + source});
        }
        const std::vector<Json> entries = m_codec.parseLines(*content);
        if (entries.empty() || !m_codec.isHeader(entries.front())) {
            return std::unexpected(Error{"fork_failed", entries.empty()
                                                             ? "Cannot fork: source session file is empty or invalid: " + source
                                                             : "Cannot fork: source session has no header: " + source});
        }
        const std::string dir = sessionDir ? resolve(*sessionDir) : defaultSessionDir(cwd);
        if (!m_files.exists(dir)) {
            if (auto created = m_files.createDirectories(dir); !created) {
                return std::unexpected(created.error());
            }
        }
        const std::string newId = id ? *id : m_ids.next();
        const std::string timestamp = m_iso.format(m_clock.nowMs());
        std::string stamp = timestamp;
        std::replace(stamp.begin(), stamp.end(), ':', '-');
        std::replace(stamp.begin(), stamp.end(), '.', '-');
        const std::string file = dir + "/" + stamp + "_" + newId + ".jsonl";
        if (m_files.exists(file)) {
            return std::unexpected(Error{"EEXIST", "EEXIST: file already exists, open '" + file + "'"});
        }
        Json header = Json::object();
        header["type"] = "session";
        header["version"] = 3;
        header["id"] = newId;
        header["timestamp"] = timestamp;
        header["cwd"] = cwd;
        header["parentSession"] = source;
        std::string text = m_codec.line(header);
        for (const auto& entry : entries) {
            if (!m_codec.isHeader(entry)) {
                text += m_codec.line(entry);
            }
        }
        if (auto written = m_files.writeFile(file, text); !written) {
            return std::unexpected(written.error());
        }
        SessionManagerOptions options;
        options.cwd = cwd;
        options.sessionDir = dir;
        options.sessionFile = file;
        return m_factory.create(options);
    }

    std::optional<std::string> findById(const std::string& cwd, const std::string& id, const std::optional<std::string>& sessionDir) override {
        const std::string dir = sessionDir ? resolve(*sessionDir) : defaultSessionDir(cwd);
        const bool filterCwd = sessionDir && dir != defaultDirPath(cwd);
        const std::string resolvedCwd = resolve(cwd);
        for (const auto& path : sessionFiles(dir)) {
            const auto header = readHeader(path);
            if (!header || header->id != id) {
                continue;
            }
            if (filterCwd && !cwdMatches(header->cwd, resolvedCwd)) {
                continue;
            }
            return path;
        }
        return std::nullopt;
    }

    std::optional<std::string> findMostRecent(const std::string& sessionDir, const std::optional<std::string>& cwd) override {
        const std::string dir = resolve(sessionDir);
        std::vector<std::pair<std::int64_t, std::string>> files;
        for (const auto& path : sessionFiles(dir)) {
            if (const auto info = m_files.stat(path)) {
                files.emplace_back(info->mtimeMs, path);
            }
        }
        std::stable_sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
            return left.first > right.first;
        });
        const std::optional<std::string> resolvedCwd = cwd ? std::optional<std::string>(resolve(*cwd)) : std::nullopt;
        for (const auto& [mtime, path] : files) {
            const auto header = readHeader(path);
            if (header && (!resolvedCwd || cwdMatches(header->cwd, *resolvedCwd))) {
                return path;
            }
        }
        return std::nullopt;
    }

    std::vector<SessionInfo> list(const std::string& cwd, const std::optional<std::string>& sessionDir) override {
        const std::string dir = sessionDir ? resolve(*sessionDir) : defaultSessionDir(cwd);
        const bool filterCwd = sessionDir && dir != defaultDirPath(cwd);
        const std::string resolvedCwd = resolve(cwd);
        std::vector<SessionInfo> infos;
        for (auto& info : infosOf(dir)) {
            if (!filterCwd || cwdMatches(info.cwd, resolvedCwd)) {
                infos.push_back(std::move(info));
            }
        }
        sortNewestFirst(infos);
        return infos;
    }

    std::vector<SessionInfo> listAll(const std::optional<std::string>& sessionDir) override {
        std::vector<SessionInfo> infos;
        if (sessionDir) {
            infos = infosOf(resolve(*sessionDir));
            sortNewestFirst(infos);
            return infos;
        }
        const std::string root = resolve(m_agentDir) + "/sessions";
        auto names = m_files.listDirectory(root);
        if (!names) {
            return infos;
        }
        for (const auto& name : *names) {
            const auto info = m_files.stat(root + "/" + name);
            if (!info || !info->isDirectory) {
                continue;
            }
            for (auto& session : infosOf(root + "/" + name)) {
                infos.push_back(std::move(session));
            }
        }
        sortNewestFirst(infos);
        return infos;
    }

private:
    std::string resolve(const std::string& path) const {
        return m_paths.resolveToCwd(path, "/");
    }

    std::string defaultDirPath(const std::string& cwd) const {
        std::string safe = resolve(cwd);
        if (!safe.empty() && (safe.front() == '/' || safe.front() == '\\')) {
            safe.erase(0, 1);
        }
        for (char& c : safe) {
            if (c == '/' || c == '\\' || c == ':') {
                c = '-';
            }
        }
        return resolve(m_agentDir) + "/sessions/--" + safe + "--";
    }

    std::string directoryOf(const std::string& path) const {
        const auto slash = path.find_last_of('/');
        return slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
    }

    std::optional<SessionHeader> readHeader(const std::string& path) {
        auto prefix = m_files.readFilePrefix(path, MaxHeaderScanBytes);
        if (!prefix) {
            return std::nullopt;
        }
        std::size_t start = 0;
        while (start < prefix->size()) {
            const std::size_t end = prefix->find('\n', start);
            const std::string line = prefix->substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (auto json = m_codec.parseLine(line)) {
                if (m_codec.isHeader(*json) && json->contains("id") && (*json)["id"].is_string()) {
                    return m_codec.headerFromJson(*json);
                }
                return std::nullopt;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return std::nullopt;
    }

    bool cwdMatches(const std::string& headerCwd, const std::string& resolvedCwd) const {
        return !headerCwd.empty() && resolve(headerCwd) == resolvedCwd;
    }

    std::vector<std::string> sessionFiles(const std::string& dir) {
        auto names = m_files.listDirectory(dir);
        std::vector<std::string> files;
        if (!names) {
            return files;
        }
        for (const auto& name : *names) {
            if (name.size() > 6 && name.substr(name.size() - 6) == ".jsonl") {
                files.push_back(dir + "/" + name);
            }
        }
        return files;
    }

    std::vector<SessionInfo> infosOf(const std::string& dir) {
        std::vector<std::string> files = sessionFiles(dir);
        std::sort(files.begin(), files.end(), std::greater<>());
        std::vector<SessionInfo> infos;
        for (const auto& path : files) {
            auto content = m_files.readFile(path);
            if (!content) {
                continue;
            }
            const auto stat = m_files.stat(path);
            if (auto info = m_infos.build(path, *content, stat ? stat->mtimeMs : 0)) {
                infos.push_back(std::move(*info));
            }
        }
        return infos;
    }

    void sortNewestFirst(std::vector<SessionInfo>& infos) const {
        std::stable_sort(infos.begin(), infos.end(), [](const SessionInfo& left, const SessionInfo& right) {
            return left.modifiedMs > right.modifiedMs;
        });
    }

    std::string m_agentDir;
    IFileSystem& m_files;
    const IClock& m_clock;
    IIdGenerator& m_ids;
    ISessionManagerFactory& m_factory;
    PathResolver m_paths;
    SessionEntryCodec m_codec;
    SessionInfoBuilder m_infos;
    IsoTimestamp m_iso;
};
