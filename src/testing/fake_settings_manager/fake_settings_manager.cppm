export module pi.testing.fake_settings_manager;

import std;
export import pi.session.i_settings_manager;

/** In-memory ISettingsManager: writes change the effective settings directly, nothing is saved. */
export class FakeSettingsManager : public ISettingsManager {
public:
    explicit FakeSettingsManager(Json initial = Json::object())
        : m_settings(std::move(initial)) {}

    Json settings() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_settings;
    }

    Json globalSettings() const override {
        return settings();
    }

    Json projectSettings() const override {
        return Json::object();
    }

    SettingsView view() const override {
        return SettingsView(settings());
    }

    bool projectTrusted() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_trusted;
    }

    Result<void> setProjectTrusted(bool trusted) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_trusted = trusted;
        return {};
    }
    void reload() override {}

    void applyOverrides(const Json& overrides) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_settings.update(overrides, true);
    }

    Result<void> setGlobal(const std::string& field, const Json& value) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_settings[field] = value;
        return {};
    }

    Result<void> setGlobalNested(const std::string& field, const std::string& key, const Json& value) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_settings[field][key] = value;
        return {};
    }

    Result<void> removeGlobal(const std::string& field) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_settings.erase(field);
        return {};
    }

    Result<void> setProject(const std::string& field, const Json& value) override {
        return setGlobal(field, value);
    }

    Result<void> removeProject(const std::string& field) override {
        return removeGlobal(field);
    }

    std::vector<SettingsError> drainErrors() override {
        return {};
    }

    Result<std::string> getOrCreateDeviceId() override {
        return std::string("device");
    }

private:
    mutable std::mutex m_mutex;
    Json m_settings;
    bool m_trusted = true;
};
