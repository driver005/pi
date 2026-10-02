export module pi.support.project_trust_resolver;

import std;
export import pi.session.i_project_trust_store;
export import pi.support.project_trust_probe;

/**
 * Decides whether the project in a cwd is trusted, in this order: explicit override, projects
 * with nothing to gate are trusted, a plugin's project_trust answer, the stored decision, the
 * defaultProjectTrust setting ("always" / "never"), and finally "ask" which a headless host
 * answers with "not trusted". Port of resolveProjectTrusted in core/project-trust.ts.
 */
export class ProjectTrustResolver {
public:
    using Handler = std::function<std::optional<std::pair<bool, bool>>(const std::string& cwd)>;

    ProjectTrustResolver(IProjectTrustStore& store, ProjectTrustProbe& probe);

    /**
     * handler returns {trusted, remember} or nullopt to abstain. defaultProjectTrust is the
     * setting value ("ask", "always", "never").
     */
    Result<bool> resolve(const std::string& cwd, std::optional<bool> override,
                         const std::string& defaultProjectTrust, const Handler& handler);

private:
    IProjectTrustStore& m_store;
    ProjectTrustProbe& m_probe;
};

ProjectTrustResolver::ProjectTrustResolver(IProjectTrustStore& store, ProjectTrustProbe& probe)
    : m_store(store), m_probe(probe) {}

Result<bool> ProjectTrustResolver::resolve(const std::string& cwd, std::optional<bool> override,
                                           const std::string& defaultProjectTrust, const Handler& handler) {
    if (override) {
        return *override;
    }
    if (!m_probe.requiresTrust(cwd)) {
        return true;
    }
    if (handler) {
        if (const auto verdict = handler(cwd)) {
            if (verdict->second) {
                if (auto saved = m_store.set(cwd, verdict->first); !saved) {
                    return std::unexpected(saved.error());
                }
            }
            return verdict->first;
        }
    }
    auto stored = m_store.get(cwd);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    if (stored->has_value()) {
        return **stored;
    }
    if (defaultProjectTrust == "always") {
        return true;
    }
    return false;
}
