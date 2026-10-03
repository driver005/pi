export module pi.session.agent_session_runtime;

import std;
export import pi.platform.i_file_system;
export import pi.session.i_agent_session_runtime;
export import pi.session.i_session_runtime_factory;
export import pi.session.i_session_store;
export import pi.support.path_resolver;

/**
 * IAgentSessionRuntime over a runtime factory: every replacement stops and disposes the current
 * session, opens the next session tree through the session store and asks the factory for a new
 * session around it. If the factory fails, the error is returned and the disposed session stays
 * in place; the caller starts over with a new session. Port of AgentSessionRuntime.
 */
export class AgentSessionRuntime : public IAgentSessionRuntime {
public:
    AgentSessionRuntime(ISessionRuntimeFactory& factory, ISessionStore& store, IFileSystem& files, std::unique_ptr<ISessionRuntimeHandle> initial)
        : m_factory(factory),
          m_store(store),
          m_files(files),
          m_paths(files.homeDirectory()),
          m_handle(std::move(initial)) {}

    IAgentSession& session() override {
        return m_handle->session();
    }

    std::string cwd() const override {
        return m_handle->cwd();
    }

    std::vector<RuntimeDiagnostic> diagnostics() const override {
        return m_handle->diagnostics();
    }

    Result<void> newSession(const std::optional<std::string>& parentSession) override {
        const std::optional<std::string> previous = m_handle->sessionManager().sessionFile();
        const std::string sessionDir = m_handle->sessionManager().sessionDir();
        const bool persisted = m_handle->sessionManager().isPersisted();
        auto manager = persisted ? m_store.create(m_handle->cwd(), sessionDir, std::nullopt, parentSession)
                                 : m_store.inMemory(m_handle->cwd());
        if (!manager) {
            return std::unexpected(manager.error());
        }
        if (!persisted && parentSession) {
            if (auto started = (*manager)->newSession(std::nullopt, parentSession); !started) {
                return std::unexpected(started.error());
            }
        }
        teardown();
        return replace(std::move(*manager), "new", previous);
    }

    Result<void> switchSession(const std::string& sessionPath, const std::optional<std::string>& cwdOverride) override {
        const std::optional<std::string> previous = m_handle->sessionManager().sessionFile();
        auto manager = m_store.open(sessionPath, std::nullopt, cwdOverride);
        if (!manager) {
            return std::unexpected(manager.error());
        }
        if (auto valid = checkCwd(**manager); !valid) {
            return valid;
        }
        teardown();
        return replace(std::move(*manager), "resume", previous);
    }

    Result<ForkResult> fork(const std::string& entryId, ForkPosition position) override {
        const auto selected = m_handle->sessionManager().entry(entryId);
        if (!selected) {
            return std::unexpected(Error{"invalid_entry", "Invalid entry ID for forking"});
        }
        ForkResult result;
        const auto leaf = forkLeaf(*selected, position, result.selectedText);
        if (!leaf) {
            return std::unexpected(leaf.error());
        }
        const auto forked = m_handle->sessionManager().isPersisted() ? forkPersisted(*leaf) : forkInMemory(*leaf);
        if (!forked) {
            return std::unexpected(forked.error());
        }
        return result;
    }

    Result<void> importFromJsonl(const std::string& inputPath, const std::optional<std::string>& cwdOverride) override {
        const std::string resolved = m_paths.resolveToCwd(inputPath, m_handle->cwd());
        if (!m_files.exists(resolved)) {
            return std::unexpected(Error{"file_not_found", "File not found: " + resolved});
        }
        const std::string sessionDir = m_handle->sessionManager().sessionDir();
        if (auto created = m_files.createDirectories(sessionDir); !created) {
            return created;
        }
        const std::string direct = sessionDir + "/" + std::filesystem::path(resolved).filename().string();
        const bool alreadyStored = direct == resolved;
        const std::string destination = alreadyStored ? resolved : uniqueDestination(sessionDir, resolved);
        const std::optional<std::string> previous = m_handle->sessionManager().sessionFile();
        if (!alreadyStored) {
            const auto content = m_files.readFile(resolved);
            if (!content) {
                return std::unexpected(content.error());
            }
            if (auto written = m_files.writeFile(destination, *content); !written) {
                return written;
            }
        }
        auto manager = m_store.open(destination, sessionDir, cwdOverride);
        if (!manager) {
            return std::unexpected(manager.error());
        }
        if (auto valid = checkCwd(**manager); !valid) {
            return valid;
        }
        teardown();
        return replace(std::move(*manager), "resume", previous);
    }

    void dispose() override {
        m_handle->session().abort();
        m_handle->session().waitForIdle();
        m_handle->session().dispose();
    }

private:
    void teardown() {
        // Settle any active response first so the aborted turn is persisted before the swap.
        m_handle->session().abort();
        m_handle->session().waitForIdle();
        m_handle->session().dispose();
    }

    Result<void> replace(std::unique_ptr<ISessionManager> manager, const std::string& reason, const std::optional<std::string>& previousFile) {
        SessionRuntimeRequest request;
        request.cwd = manager->cwd();
        request.agentDir = m_handle->agentDir();
        request.sessionManager = std::move(manager);
        request.startReason = reason;
        request.previousSessionFile = previousFile;
        auto next = m_factory.create(std::move(request));
        if (!next) {
            return std::unexpected(next.error());
        }
        m_handle = std::move(*next);
        return {};
    }

    Result<void> checkCwd(const ISessionManager& manager) const {
        const std::string stored = manager.cwd();
        const auto file = manager.sessionFile();
        if (!file || stored.empty() || m_files.exists(stored)) {
            return {};
        }
        return std::unexpected(Error{"missing_session_cwd", "Stored session working directory does not exist: " + stored +
                                                                "\nSession file: " + *file +
                                                                "\nCurrent working directory: " + m_handle->cwd()});
    }

    Result<std::optional<std::string>> forkLeaf(const SessionEntry& selected, ForkPosition position, std::optional<std::string>& selectedText) const {
        if (position == ForkPosition::At) {
            return std::optional<std::string>(selected.id);
        }
        const bool userMessage = selected.type == "message" && selected.body.contains("message") &&
                                 selected.body["message"].value("role", "") == "user";
        if (!userMessage) {
            return std::unexpected(Error{"invalid_entry", "Invalid entry ID for forking"});
        }
        selectedText = userText(selected);
        return selected.parentId;
    }

    Result<void> forkPersisted(const std::optional<std::string>& leaf) {
        ISessionManager& current = m_handle->sessionManager();
        const auto currentFile = current.sessionFile();
        if (!currentFile) {
            return std::unexpected(Error{"no_session_file", "Persisted session is missing a session file"});
        }
        const std::string sessionDir = current.sessionDir();
        if (!leaf) {
            auto manager = m_store.create(m_handle->cwd(), sessionDir, std::nullopt, *currentFile);
            if (!manager) {
                return std::unexpected(manager.error());
            }
            teardown();
            return replace(std::move(*manager), "fork", currentFile);
        }
        if (!m_files.exists(*currentFile)) {
            return std::unexpected(Error{"not_saved", "This session has not been saved yet. Send a message before cloning or forking it."});
        }
        auto manager = m_store.open(*currentFile, sessionDir, std::nullopt);
        if (!manager) {
            return std::unexpected(manager.error());
        }
        const auto branched = (*manager)->createBranchedSession(*leaf);
        if (!branched || !branched->has_value()) {
            return std::unexpected(Error{"fork_failed", "Failed to create forked session"});
        }
        teardown();
        return replace(std::move(*manager), "fork", currentFile);
    }

    Result<void> forkInMemory(const std::optional<std::string>& leaf) {
        const std::optional<std::string> previous = m_handle->sessionManager().sessionFile();
        teardown();
        std::unique_ptr<ISessionManager> manager = m_handle->releaseSessionManager();
        if (!leaf) {
            if (auto started = manager->newSession(std::nullopt, previous); !started) {
                return std::unexpected(started.error());
            }
        } else if (auto branched = manager->createBranchedSession(*leaf); !branched) {
            return std::unexpected(branched.error());
        }
        return replace(std::move(manager), "fork", previous);
    }

    std::string uniqueDestination(const std::string& sessionDir, const std::string& source) const {
        const std::filesystem::path name = std::filesystem::path(source).filename();
        std::string destination = sessionDir + "/" + name.string();
        int suffix = 1;
        while (m_files.exists(destination)) {
            destination = sessionDir + "/" + name.stem().string() + "-" + std::to_string(suffix++) + name.extension().string();
        }
        return destination;
    }

    std::string userText(const SessionEntry& entry) const {
        const Json& content = entry.body["message"]["content"];
        if (content.is_string()) {
            return content.get<std::string>();
        }
        std::string text;
        for (const auto& block : content) {
            if (block.value("type", "") == "text" && block.contains("text") && block["text"].is_string()) {
                text += block["text"].get<std::string>();
            }
        }
        return text;
    }

    ISessionRuntimeFactory& m_factory;
    ISessionStore& m_store;
    IFileSystem& m_files;
    PathResolver m_paths;
    std::unique_ptr<ISessionRuntimeHandle> m_handle;
};
