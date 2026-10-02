export module pi.testing.fake_resource_loader;

import std;
export import pi.session.i_resource_loader;

/** IResourceLoader that returns whatever the test sets and counts reloads. */
export class FakeResourceLoader : public IResourceLoader {
public:
    void set(LoadedResources resources);
    int reloads() const;

    LoadedResources resources() const override;
    Result<void> reload() override;

private:
    mutable std::mutex m_mutex;
    LoadedResources m_resources;
    int m_reloads = 0;
};

void FakeResourceLoader::set(LoadedResources resources) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_resources = std::move(resources);
}

int FakeResourceLoader::reloads() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_reloads;
}

LoadedResources FakeResourceLoader::resources() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_resources;
}

Result<void> FakeResourceLoader::reload() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_reloads;
    return {};
}
