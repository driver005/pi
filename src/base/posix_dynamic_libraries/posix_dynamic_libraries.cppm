module;

#include <dlfcn.h>

#include <cstdint>

export module pi.base.posix_dynamic_libraries;

import std;
export import pi.platform.i_dynamic_libraries;

/** IDynamicLibraries over dlopen (RTLD_NOW | RTLD_LOCAL). Libraries still open are closed on destruction. */
export class PosixDynamicLibraries : public IDynamicLibraries {
public:
    ~PosixDynamicLibraries() override {
        for (const auto& entry : m_handles) {
            dlclose(entry.second);
        }
    }

    Result<std::uint64_t> open(const std::string& path) override {
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            const char* reason = dlerror();
            return std::unexpected(Error{"dlopen", reason != nullptr ? reason : "dlopen failed: " + path});
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        const std::uint64_t id = m_nextId++;
        m_handles[id] = handle;
        return id;
    }

    Result<void*> symbol(std::uint64_t library, const std::string& name) override {
        void* handle = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_handles.find(library);
            if (found == m_handles.end()) {
                return std::unexpected(Error{"dlsym", "library is not open"});
            }
            handle = found->second;
        }
        void* address = dlsym(handle, name.c_str());
        if (address == nullptr) {
            return std::unexpected(Error{"dlsym", "missing symbol " + name});
        }
        return address;
    }

    void close(std::uint64_t library) override {
        void* handle = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_handles.find(library);
            if (found == m_handles.end()) {
                return;
            }
            handle = found->second;
            m_handles.erase(found);
        }
        dlclose(handle);
    }

private:
    std::mutex m_mutex;
    std::uint64_t m_nextId = 1;
    std::map<std::uint64_t, void*> m_handles;
};
