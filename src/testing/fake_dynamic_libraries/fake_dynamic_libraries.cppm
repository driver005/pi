export module pi.testing.fake_dynamic_libraries;

import std;
export import pi.platform.i_dynamic_libraries;

/** IDynamicLibraries over in-process symbol tables, so plugins can be plain functions in a test. */
export class FakeDynamicLibraries : public IDynamicLibraries {
public:
    /** Makes `path` loadable with these symbols. */
    void provide(const std::string& path, std::map<std::string, void*> symbols) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_available[path] = std::move(symbols);
    }

    std::vector<std::string> opened() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_opened;
    }

    int closed() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

    Result<std::uint64_t> open(const std::string& path) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_available.contains(path)) {
            return std::unexpected(Error{"dlopen", path + ": cannot open shared object file"});
        }
        const std::uint64_t id = m_nextId++;
        m_open[id] = path;
        m_opened.push_back(path);
        return id;
    }

    Result<void*> symbol(std::uint64_t library, const std::string& name) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto path = m_open.find(library);
        if (path == m_open.end()) {
            return std::unexpected(Error{"dlsym", "library is not open"});
        }
        const auto& symbols = m_available[path->second];
        const auto found = symbols.find(name);
        if (found == symbols.end()) {
            return std::unexpected(Error{"dlsym", "missing symbol " + name});
        }
        return found->second;
    }

    void close(std::uint64_t library) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_open.erase(library) > 0) {
            ++m_closed;
        }
    }

private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::map<std::string, void*>> m_available;
    std::map<std::uint64_t, std::string> m_open;
    std::vector<std::string> m_opened;
    std::uint64_t m_nextId = 1;
    int m_closed = 0;
};
