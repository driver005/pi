export module pi.ai.memory_credential_store;

import std;
export import pi.provider.i_credential_store;
export import pi.support.config_value_resolver;

/** Process-local ICredentialStore; nothing is persisted. */
export class MemoryCredentialStore : public ICredentialStore {
public:
    explicit MemoryCredentialStore(ConfigValueResolver& resolver);

    Result<std::optional<Credential>> read(const std::string& providerId) override;
    Result<std::vector<CredentialInfo>> list() override;
    Result<std::optional<Credential>> modify(const std::string& providerId,
                                             const Modifier& fn) override;
    Result<void> remove(const std::string& providerId) override;

private:
    ConfigValueResolver& m_resolver;
    std::mutex m_mutex;
    std::vector<std::pair<std::string, Credential>> m_entries;
};

MemoryCredentialStore::MemoryCredentialStore(ConfigValueResolver& resolver) : m_resolver(resolver) {}

Result<std::optional<Credential>> MemoryCredentialStore::read(const std::string& providerId) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [id, credential] : m_entries) {
        if (id == providerId) {
            Credential out = credential;
            if (out.type == CredentialType::ApiKey && out.key) {
                out.key = m_resolver.resolve(*out.key, out.env.value_or(ConfigValueResolver::Env{}));
            }
            return std::optional<Credential>(std::move(out));
        }
    }
    return std::optional<Credential>();
}

Result<std::vector<CredentialInfo>> MemoryCredentialStore::list() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<CredentialInfo> infos;
    for (const auto& [id, credential] : m_entries) {
        infos.push_back(CredentialInfo{id, credential.type});
    }
    return infos;
}

Result<std::optional<Credential>> MemoryCredentialStore::modify(const std::string& providerId,
                                                                const Modifier& fn) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto found = std::find_if(m_entries.begin(), m_entries.end(),
                              [&](const auto& entry) { return entry.first == providerId; });
    const std::optional<Credential> current =
        found == m_entries.end() ? std::nullopt : std::optional<Credential>(found->second);
    auto next = fn(current);
    if (!next) {
        return std::unexpected(next.error());
    }
    if (!next->has_value()) {
        return current;
    }
    if (found == m_entries.end()) {
        m_entries.emplace_back(providerId, **next);
    } else {
        found->second = **next;
    }
    return next;
}

Result<void> MemoryCredentialStore::remove(const std::string& providerId) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::erase_if(m_entries, [&](const auto& entry) { return entry.first == providerId; });
    return {};
}
