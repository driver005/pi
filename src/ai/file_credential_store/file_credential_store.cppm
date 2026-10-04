export module pi.ai.file_credential_store;

import std;
export import pi.platform.i_file_lock;
export import pi.platform.i_file_system;
export import pi.provider.i_credential_store;
export import pi.support.config_value_resolver;
export import pi.support.credential_codec;

/**
 * ICredentialStore over an auth.json file (default ~/.pi/agent/auth.json), format-compatible
 * with the TypeScript AuthStorage: private file mode, pretty-printed JSON, writes under the same
 * lock-directory protocol. API key values are resolved on read.
 */
export class FileCredentialStore : public ICredentialStore {
public:
    FileCredentialStore(std::string authPath, IFileSystem& files, IFileLock& lock, ConfigValueResolver& resolver)
        : m_path(std::move(authPath)),
          m_files(files),
          m_lock(lock),
          m_resolver(resolver) {}

    Result<std::optional<Credential>> read(const std::string& providerId) override {
        auto entries = load();
        if (!entries) {
            return std::unexpected(entries.error());
        }
        for (const auto& [id, credential] : *entries) {
            if (id != providerId) {
                continue;
            }
            Credential out = credential;
            if (out.type == CredentialType::ApiKey && out.key) {
                out.key = m_resolver.resolve(*out.key, out.env.value_or(ConfigValueResolver::Env{}));
            }
            return std::optional<Credential>(std::move(out));
        }
        return std::optional<Credential>();
    }

    Result<std::vector<CredentialInfo>> list() override {
        auto entries = load();
        if (!entries) {
            return std::unexpected(entries.error());
        }
        std::vector<CredentialInfo> infos;
        for (const auto& [id, credential] : *entries) {
            infos.push_back(CredentialInfo{id, credential.type});
        }
        return infos;
    }

    Result<std::optional<Credential>> modify(const std::string& providerId, const Modifier& fn) override {
        if (auto ready = prepare(); !ready) {
            return std::unexpected(ready.error());
        }
        Result<std::optional<Credential>> result = std::optional<Credential>();
        auto outcome = m_lock.withLock(m_path, [&]() -> Result<void> {
            auto entries = loadLocked();
            if (!entries) {
                result = std::unexpected(entries.error());
                return {};
            }
            auto found = std::find_if(entries->begin(), entries->end(),
                                      [&](const auto& entry) { return entry.first == providerId; });
            const std::optional<Credential> current =
                found == entries->end() ? std::nullopt : std::optional<Credential>(found->second);
            auto next = fn(current);
            if (!next) {
                result = std::unexpected(next.error());
                return {};
            }
            if (!next->has_value()) {
                result = current;
                return {};
            }
            if (found == entries->end()) {
                entries->emplace_back(providerId, **next);
            } else {
                found->second = **next;
            }
            if (auto saved = save(*entries); !saved) {
                result = std::unexpected(saved.error());
                return {};
            }
            result = next;
            return {};
        });
        if (!outcome) {
            return std::unexpected(outcome.error());
        }
        return result;
    }

    Result<void> remove(const std::string& providerId) override {
        if (auto ready = prepare(); !ready) {
            return ready;
        }
        Result<void> result;
        auto outcome = m_lock.withLock(m_path, [&]() -> Result<void> {
            auto entries = loadLocked();
            if (!entries) {
                result = std::unexpected(entries.error());
                return {};
            }
            std::erase_if(*entries, [&](const auto& entry) { return entry.first == providerId; });
            result = save(*entries);
            return {};
        });
        if (!outcome) {
            return outcome;
        }
        return result;
    }

private:
    using Entries = std::vector<std::pair<std::string, Credential>>;

    Result<Entries> load() {
        auto entries = loadLocked();
        if (entries) {
            return entries;
        }
        // A concurrent writer may have been mid-write; retry once under the lock.
        Result<Entries> locked = Entries{};
        auto outcome = m_lock.withLock(m_path, [&]() -> Result<void> {
            locked = loadLocked();
            return {};
        });
        if (!outcome) {
            return std::unexpected(outcome.error());
        }
        return locked;
    }

    Result<Entries> loadLocked() {
        auto text = m_files.readFile(m_path);
        if (!text) {
            if (text.error().code == "ENOENT") {
                return Entries{};
            }
            return std::unexpected(text.error());
        }
        return m_codec.parseDocument(*text);
    }

    Result<void> prepare() {
        if (auto created = m_files.createPrivateDirectories(parentDirectory()); !created) {
            return created;
        }
        if (!m_files.exists(m_path)) {
            return m_files.writeFilePrivate(m_path, "{}");
        }
        return {};
    }

    Result<void> save(const Entries& entries) {
        return m_files.writeFilePrivate(m_path, m_codec.serializeDocument(entries));
    }

    std::string parentDirectory() const {
        const auto slash = m_path.find_last_of('/');
        return slash == std::string::npos ? "." : m_path.substr(0, slash == 0 ? 1 : slash);
    }

    std::string m_path;
    IFileSystem& m_files;
    IFileLock& m_lock;
    ConfigValueResolver& m_resolver;
    CredentialCodec m_codec;
};
