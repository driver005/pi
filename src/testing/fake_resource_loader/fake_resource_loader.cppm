export module pi.testing.fake_resource_loader;

import std;
export import pi.session.i_resource_loader;

/** IResourceLoader that returns whatever the test sets and counts reloads. */
export class FakeResourceLoader : public IResourceLoader {
public:
    void set(LoadedResources resources) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_resources = std::move(resources);
    }

    int reloads() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_reloads;
    }

    LoadedResources resources() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_resources;
    }

    Result<void> reload() override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        ++m_reloads;
        return {};
    }

private:
    mutable std::mutex m_mutex;
    LoadedResources m_resources;
    int m_reloads = 0;
};
