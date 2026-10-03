export module pi.support.mcp_oauth_store;

import std;
export import pi.platform.i_crypto;
export import pi.platform.i_file_lock;
export import pi.platform.i_file_system;
export import pi.support.url_parser;
export import pi.types.json;
export import pi.types.result;

/**
 * Per-server OAuth state of remote MCP servers (client registration, tokens, discovery, a pending PKCE verifier) in
 * `mcp-auth.json`, the file the TypeScript implementation writes: an object keyed by `mcp__<server with - as _>|<normalized
 * URL>`, each value `{serverUrl, clientInformation?, tokens?, tokensExpireAt?, codeVerifier?, oauthState?, discovery?}`.
 * State written under the old key (the URL alone) is taken over by the first server to load it. Reads and writes run under
 * the file lock; state stored for another URL under the same key is ignored, so credentials never reach another server.
 * Port of McpOAuthCredentialStore in extensions/mcp/oauth.ts.
 */
export class McpOauthStore {
public:
    /** `lockDir` holds the refresh lock files (shared with the TypeScript implementation). */
    McpOauthStore(std::string path, std::string lockDir, IFileSystem& files, IFileLock& lock, const ICrypto& crypto)
        : m_path(std::move(path)),
          m_lockDir(std::move(lockDir)),
          m_files(files),
          m_lock(lock),
          m_crypto(crypto) {}

    /** The stored state of a server, or nullopt when it has none for this URL. */
    Result<std::optional<Json>> load(const std::string& name, const std::string& serverUrl) {
        auto keys = storeKeys(name, serverUrl);
        if (!keys) {
            return std::unexpected(keys.error());
        }
        std::optional<Json> found;
        auto done = update([&](Json& states) {
            const Json mine = valueOf(states, keys->first);
            if (mine.is_object()) {
                found = mine;
                return false;
            }
            const Json legacy = valueOf(states, keys->second);
            if (!legacy.is_object()) {
                return false;
            }
            states[keys->first] = legacy;
            states.erase(keys->second);
            found = legacy;
            return true;
        });
        if (!done) {
            return std::unexpected(done.error());
        }
        return owned(found, keys->second);
    }

    Result<void> save(const std::string& name, const std::string& serverUrl, const Json& state) {
        auto keys = storeKeys(name, serverUrl);
        if (!keys) {
            return std::unexpected(keys.error());
        }
        return update([&](Json& states) {
            states[keys->first] = state;
            return true;
        });
    }

    /** True when credentials were stored for the server; also drops old-key state the server would take over. */
    Result<bool> remove(const std::string& name, const std::string& serverUrl) {
        auto keys = storeKeys(name, serverUrl);
        if (!keys) {
            return std::unexpected(keys.error());
        }
        bool removed = false;
        auto done = update([&](Json& states) {
            for (const std::string& key : {keys->first, keys->second}) {
                if (states.contains(key)) {
                    states.erase(key);
                    removed = true;
                    return true;
                }
            }
            return false;
        });
        if (!done) {
            return std::unexpected(done.error());
        }
        return removed;
    }

    /** Runs `fn` while no other process refreshes this server's tokens. */
    Result<void> withRefreshLock(const std::string& name, const std::string& serverUrl, const std::function<Result<void>()>& fn) {
        auto keys = storeKeys(name, serverUrl);
        if (!keys) {
            return std::unexpected(keys.error());
        }
        if (m_lockDir.empty()) {
            return fn();
        }
        if (auto created = m_files.createPrivateDirectories(m_lockDir); !created) {
            return created;
        }
        return m_lock.withLock(m_lockDir + "/mcp-auth-refresh-" + lockName(keys->first), fn);
    }

    /** `mcp__<server>` with `-` replaced by `_`, the namespace of the server's tools. */
    std::string namespaceOf(const std::string& name) const {
        std::string value = name;
        std::replace(value.begin(), value.end(), '-', '_');
        return "mcp__" + value;
    }

private:
    /** The key of a server's state and the old key (the normalized URL alone). */
    Result<std::pair<std::string, std::string>> storeKeys(const std::string& name, const std::string& serverUrl) const {
        auto url = m_urls.normalize(serverUrl);
        if (!url) {
            return std::unexpected(url.error());
        }
        return std::make_pair(namespaceOf(name) + "|" + *url, *url);
    }

    template <typename Mutator>
    Result<void> update(const Mutator& mutate) {
        if (auto created = m_files.createPrivateDirectories(parentDirectory()); !created) {
            return created;
        }
        if (!m_files.exists(m_path)) {
            if (auto created = m_files.writeFilePrivate(m_path, "{}"); !created) {
                return created;
            }
        }
        Result<void> result;
        auto locked = m_lock.withLock(m_path, [&]() -> Result<void> {
            auto states = read();
            if (!states) {
                result = std::unexpected(states.error());
                return {};
            }
            if (mutate(*states)) {
                result = m_files.writeFilePrivate(m_path, states->dump(2) + "\n");
            }
            return {};
        });
        if (!locked) {
            return locked;
        }
        return result;
    }

    Result<Json> read() {
        auto text = m_files.readFile(m_path);
        if (!text) {
            return text.error().code == "ENOENT" ? Result<Json>(Json::object()) : std::unexpected(text.error());
        }
        if (text->find_first_not_of(" \t\r\n") == std::string::npos) {
            return Json::object();
        }
        const Json parsed = Json::parse(*text, nullptr, false);
        if (parsed.is_discarded()) {
            return std::unexpected(Error{"invalid_auth_file", m_path + " is not valid JSON"});
        }
        return parsed.is_object() ? parsed : Json::object();
    }

    Json valueOf(const Json& states, const std::string& key) const {
        return states.contains(key) ? states[key] : Json();
    }

    /** The state when it belongs to this server URL. */
    std::optional<Json> owned(const std::optional<Json>& state, const std::string& url) const {
        if (!state || !state->is_object() || !state->contains("serverUrl") || (*state)["serverUrl"] != url) {
            return std::nullopt;
        }
        return state;
    }

    std::string lockName(const std::string& key) const {
        const std::string digest = m_crypto.sha256(key);
        std::string hex;
        for (const char byte : digest.substr(0, 8)) {
            hex += std::format("{:02x}", static_cast<unsigned char>(byte));
        }
        return hex;
    }

    std::string parentDirectory() const {
        const auto slash = m_path.find_last_of('/');
        return slash == std::string::npos ? "." : m_path.substr(0, slash == 0 ? 1 : slash);
    }

    std::string m_path;
    std::string m_lockDir;
    IFileSystem& m_files;
    IFileLock& m_lock;
    const ICrypto& m_crypto;
    UrlParser m_urls;
};
