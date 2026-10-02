export module pi.testing.fake_environment;

import std;
export import pi.platform.i_environment;

/** IEnvironment backed by a map, so tests never read or change the real environment. */
export class FakeEnvironment : public IEnvironment {
public:
    FakeEnvironment() = default;
    explicit FakeEnvironment(std::map<std::string, std::string> values);

    std::optional<std::string> get(const std::string& name) const override;
    void set(const std::string& name, const std::string& value) override;
    void unset(const std::string& name) override;
    std::map<std::string, std::string> all() const override;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::string> m_values;
};

FakeEnvironment::FakeEnvironment(std::map<std::string, std::string> values)
    : m_values(std::move(values)) {}

std::optional<std::string> FakeEnvironment::get(const std::string& name) const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_values.find(name);
    if (found == m_values.end()) {
        return std::nullopt;
    }
    return found->second;
}

void FakeEnvironment::set(const std::string& name, const std::string& value) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_values[name] = value;
}

void FakeEnvironment::unset(const std::string& name) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_values.erase(name);
}

std::map<std::string, std::string> FakeEnvironment::all() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_values;
}
