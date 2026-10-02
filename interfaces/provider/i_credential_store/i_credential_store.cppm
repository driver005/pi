export module pi.provider.i_credential_store;

import std;
export import pi.types.credential;
export import pi.types.credential_info;
export import pi.types.result;

/**
 * Credential storage keyed by provider id, one credential per provider. `modify` is the only
 * write path so every change is a serialized read-modify-write (also across processes where the
 * backing store supports it); OAuth refresh runs inside it so concurrent requests cannot refresh
 * a rotated token twice.
 */
export class ICredentialStore {
public:
    using Modifier =
        std::function<Result<std::optional<Credential>>(const std::optional<Credential>&)>;

    virtual ~ICredentialStore() = default;

    /** nullopt for a missing entry. API key values are resolved (env references, commands). */
    virtual Result<std::optional<Credential>> read(const std::string& providerId) = 0;

    /** Provider ids and types only; never resolves keys or runs commands. */
    virtual Result<std::vector<CredentialInfo>> list() = 0;

    /**
     * Calls `fn` with the stored credential (raw, unresolved) under the store's lock. fn returns
     * the new credential, or nullopt to leave the entry unchanged. Resolves with the post-write
     * credential (the unchanged one when nothing was written).
     */
    virtual Result<std::optional<Credential>> modify(const std::string& providerId,
                                                     const Modifier& fn) = 0;

    virtual Result<void> remove(const std::string& providerId) = 0;
};
