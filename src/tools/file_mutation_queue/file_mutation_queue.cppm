export module pi.tools.file_mutation_queue;

import std;
export import pi.platform.i_file_system;
export import pi.tool.i_file_mutation_queue;

/** Per-realpath mutex map (port of core/tools/file-mutation-queue.ts). */
export class FileMutationQueue : public IFileMutationQueue {
public:
    explicit FileMutationQueue(IFileSystem& fileSystem) : m_fileSystem(fileSystem) {}

    void run(const std::string& path, const std::function<void()>& action) override {
        const std::string key = m_fileSystem.realPath(std::filesystem::absolute(path).lexically_normal().string());
        const std::shared_ptr<std::mutex> lock = acquire(key);
        {
            const std::lock_guard<std::mutex> guard(*lock);
            action();
        }
        release(key);
    }

    /** Number of files currently tracked (zero when idle); for tests. */
    std::size_t trackedFiles() {
        const std::lock_guard<std::mutex> guard(m_registryMutex);
        return m_entries.size();
    }

private:
    std::shared_ptr<std::mutex> acquire(const std::string& key) {
        const std::lock_guard<std::mutex> guard(m_registryMutex);
        auto& entry = m_entries[key];
        if (entry.first == nullptr) {
            entry.first = std::make_shared<std::mutex>();
        }
        ++entry.second;
        return entry.first;
    }

    void release(const std::string& key) {
        const std::lock_guard<std::mutex> guard(m_registryMutex);
        auto found = m_entries.find(key);
        if (found != m_entries.end() && --found->second.second == 0) {
            m_entries.erase(found);
        }
    }

    IFileSystem& m_fileSystem;
    std::mutex m_registryMutex;
    std::map<std::string, std::pair<std::shared_ptr<std::mutex>, int>> m_entries;
};
