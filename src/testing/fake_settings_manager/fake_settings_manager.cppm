export module pi.testing.fake_settings_manager;

import std;
export import pi.session.i_settings_manager;

/** In-memory ISettingsManager: writes change the effective settings directly, nothing is saved. */
export class FakeSettingsManager : public ISettingsManager {
public:
    explicit FakeSettingsManager(Json initial = Json::object());

    Json settings() const override;
    Json globalSettings() const override;
    Json projectSettings() const override;
    SettingsView view() const override;
    bool projectTrusted() const override;
    Result<void> setProjectTrusted(bool trusted) override;
    void reload() override;
    void applyOverrides(const Json& overrides) override;
    Result<void> setGlobal(const std::string& field, const Json& value) override;
    Result<void> setGlobalNested(const std::string& field, const std::string& key, const Json& value) override;
    Result<void> removeGlobal(const std::string& field) override;
    Result<void> setProject(const std::string& field, const Json& value) override;
    Result<void> removeProject(const std::string& field) override;
    std::vector<SettingsError> drainErrors() override;
    Result<std::string> getOrCreateDeviceId() override;

private:
    mutable std::mutex m_mutex;
    Json m_settings;
    bool m_trusted = true;
};

FakeSettingsManager::FakeSettingsManager(Json initial) : m_settings(std::move(initial)) {}

Json FakeSettingsManager::settings() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_settings;
}

Json FakeSettingsManager::globalSettings() const {
    return settings();
}

Json FakeSettingsManager::projectSettings() const {
    return Json::object();
}

SettingsView FakeSettingsManager::view() const {
    return SettingsView(settings());
}

bool FakeSettingsManager::projectTrusted() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_trusted;
}

Result<void> FakeSettingsManager::setProjectTrusted(bool trusted) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_trusted = trusted;
    return {};
}

void FakeSettingsManager::reload() {}

void FakeSettingsManager::applyOverrides(const Json& overrides) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_settings.update(overrides, true);
}

Result<void> FakeSettingsManager::setGlobal(const std::string& field, const Json& value) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_settings[field] = value;
    return {};
}

Result<void> FakeSettingsManager::setGlobalNested(const std::string& field, const std::string& key,
                                                  const Json& value) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_settings[field][key] = value;
    return {};
}

Result<void> FakeSettingsManager::removeGlobal(const std::string& field) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_settings.erase(field);
    return {};
}

Result<void> FakeSettingsManager::setProject(const std::string& field, const Json& value) {
    return setGlobal(field, value);
}

Result<void> FakeSettingsManager::removeProject(const std::string& field) {
    return removeGlobal(field);
}

std::vector<SettingsError> FakeSettingsManager::drainErrors() {
    return {};
}

Result<std::string> FakeSettingsManager::getOrCreateDeviceId() {
    return std::string("device");
}
