export module pi.testing.fake_file_system;

import std;
export import pi.platform.i_file_system;

/** In-memory IFileSystem for tests: paths are plain strings, no symlinks, a fixed home. */
export class FakeFileSystem : public IFileSystem {
public:
    explicit FakeFileSystem(std::string home = "/home/user");

    Result<std::string> readFile(const std::string& path) override;
    Result<std::string> readFilePrefix(const std::string& path, std::size_t maxBytes) override;
    Result<void> writeFile(const std::string& path, const std::string& content) override;
    Result<void> writeFilePrivate(const std::string& path, const std::string& content) override;
    Result<void> appendFile(const std::string& path, const std::string& content) override;
    Result<void> createDirectories(const std::string& path) override;
    Result<void> createPrivateDirectories(const std::string& path) override;
    Result<void> removeFile(const std::string& path) override;
    Result<void> renameFile(const std::string& from, const std::string& to) override;
    bool exists(const std::string& path) override;
    bool isReadable(const std::string& path) override;
    bool isWritable(const std::string& path) override;
    Result<FileStat> stat(const std::string& path) override;
    Result<std::vector<std::string>> listDirectory(const std::string& path) override;
    std::string realPath(const std::string& path) override;
    std::string homeDirectory() override;

    /** Test helper: current content or empty. */
    std::string content(const std::string& path) const;
    /** Test helper: advances the clock used for mtimes. */
    void setNowMs(std::int64_t nowMs);

private:
    Error notFound(const std::string& path) const;
    std::string parentOf(const std::string& path) const;
    void addParents(const std::string& path);

    mutable std::mutex m_mutex;
    std::string m_home;
    std::int64_t m_nowMs = 1700000000000;
    std::map<std::string, std::string> m_files;
    std::map<std::string, std::int64_t> m_mtimes;
    std::set<std::string> m_directories{"/"};
};

FakeFileSystem::FakeFileSystem(std::string home) : m_home(std::move(home)) {
    addParents(m_home + "/x");
}

Error FakeFileSystem::notFound(const std::string& path) const {
    return Error{"ENOENT", "ENOENT: no such file or directory, '" + path + "'"};
}

std::string FakeFileSystem::parentOf(const std::string& path) const {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

void FakeFileSystem::addParents(const std::string& path) {
    std::string parent = parentOf(path);
    while (parent != "/" && m_directories.insert(parent).second) {
        parent = parentOf(parent);
    }
}

Result<std::string> FakeFileSystem::readFile(const std::string& path) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_directories.contains(path)) {
        return std::unexpected(Error{"EISDIR", "EISDIR: illegal operation on a directory, read"});
    }
    const auto found = m_files.find(path);
    if (found == m_files.end()) {
        return std::unexpected(notFound(path));
    }
    return found->second;
}

Result<std::string> FakeFileSystem::readFilePrefix(const std::string& path, std::size_t maxBytes) {
    auto content = readFile(path);
    if (!content) {
        return content;
    }
    return content->substr(0, maxBytes);
}

Result<void> FakeFileSystem::writeFile(const std::string& path, const std::string& content) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_directories.contains(parentOf(path))) {
        return std::unexpected(notFound(path));
    }
    m_files[path] = content;
    m_mtimes[path] = m_nowMs;
    return {};
}

Result<void> FakeFileSystem::writeFilePrivate(const std::string& path, const std::string& content) {
    return writeFile(path, content);
}

Result<void> FakeFileSystem::appendFile(const std::string& path, const std::string& content) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_directories.contains(parentOf(path))) {
        return std::unexpected(notFound(path));
    }
    m_files[path] += content;
    m_mtimes[path] = m_nowMs;
    return {};
}

Result<void> FakeFileSystem::createDirectories(const std::string& path) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_directories.insert(path);
    addParents(path);
    return {};
}

Result<void> FakeFileSystem::createPrivateDirectories(const std::string& path) {
    return createDirectories(path);
}

Result<void> FakeFileSystem::removeFile(const std::string& path) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_files.erase(path) == 0) {
        return std::unexpected(notFound(path));
    }
    m_mtimes.erase(path);
    return {};
}

Result<void> FakeFileSystem::renameFile(const std::string& from, const std::string& to) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_files.find(from);
    if (found == m_files.end()) {
        return std::unexpected(notFound(from));
    }
    m_files[to] = found->second;
    m_mtimes[to] = m_nowMs;
    m_files.erase(from);
    m_mtimes.erase(from);
    return {};
}

bool FakeFileSystem::exists(const std::string& path) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_files.contains(path) || m_directories.contains(path);
}

bool FakeFileSystem::isReadable(const std::string& path) {
    return exists(path);
}

bool FakeFileSystem::isWritable(const std::string& path) {
    return exists(path);
}

Result<FileStat> FakeFileSystem::stat(const std::string& path) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    FileStat info;
    if (m_directories.contains(path)) {
        info.isDirectory = true;
        info.mtimeMs = m_nowMs;
        return info;
    }
    const auto found = m_files.find(path);
    if (found == m_files.end()) {
        return std::unexpected(notFound(path));
    }
    info.isFile = true;
    info.size = found->second.size();
    info.mtimeMs = m_mtimes[path];
    return info;
}

Result<std::vector<std::string>> FakeFileSystem::listDirectory(const std::string& path) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_directories.contains(path)) {
        return std::unexpected(notFound(path));
    }
    std::set<std::string> names;
    const std::string prefix = path == "/" ? "/" : path + "/";
    for (const auto& entry : m_files) {
        if (entry.first.rfind(prefix, 0) == 0) {
            names.insert(entry.first.substr(prefix.size(), entry.first.find('/', prefix.size()) -
                                                               prefix.size()));
        }
    }
    for (const auto& dir : m_directories) {
        if (dir != path && dir.rfind(prefix, 0) == 0) {
            names.insert(dir.substr(prefix.size(), dir.find('/', prefix.size()) - prefix.size()));
        }
    }
    return std::vector<std::string>(names.begin(), names.end());
}

std::string FakeFileSystem::realPath(const std::string& path) {
    return path;
}

std::string FakeFileSystem::homeDirectory() {
    return m_home;
}

std::string FakeFileSystem::content(const std::string& path) const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_files.find(path);
    return found == m_files.end() ? "" : found->second;
}

void FakeFileSystem::setNowMs(std::int64_t nowMs) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_nowMs = nowMs;
}
