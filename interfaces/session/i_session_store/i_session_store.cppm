export module pi.session.i_session_store;

import std;
export import pi.session.i_session_manager;
export import pi.types.result;
export import pi.types.session_info;

/**
 * Finds, creates and lists session files. Default location is
 * <agentDir>/sessions/--<cwd with separators as dashes>--/.
 */
export class ISessionStore {
public:
    virtual ~ISessionStore() = default;

    /** Directory for a cwd's sessions (created on demand). */
    virtual std::string defaultSessionDir(const std::string& cwd) = 0;

    virtual Result<std::unique_ptr<ISessionManager>> create(
        const std::string& cwd, const std::optional<std::string>& sessionDir = std::nullopt,
        const std::optional<std::string>& id = std::nullopt,
        const std::optional<std::string>& parentSession = std::nullopt) = 0;
    virtual Result<std::unique_ptr<ISessionManager>> open(
        const std::string& path, const std::optional<std::string>& sessionDir = std::nullopt,
        const std::optional<std::string>& cwdOverride = std::nullopt) = 0;
    virtual Result<std::unique_ptr<ISessionManager>> continueRecent(
        const std::string& cwd, const std::optional<std::string>& sessionDir = std::nullopt) = 0;
    virtual Result<std::unique_ptr<ISessionManager>> inMemory(const std::string& cwd) = 0;
    /** New session in targetCwd with the full history of the source session file. */
    virtual Result<std::unique_ptr<ISessionManager>> forkFrom(
        const std::string& sourcePath, const std::string& targetCwd,
        const std::optional<std::string>& sessionDir = std::nullopt,
        const std::optional<std::string>& id = std::nullopt) = 0;

    virtual std::optional<std::string> findById(const std::string& cwd, const std::string& id,
                                                const std::optional<std::string>& sessionDir = std::nullopt) = 0;
    virtual std::optional<std::string> findMostRecent(const std::string& sessionDir,
                                                      const std::optional<std::string>& cwd = std::nullopt) = 0;
    /** Sessions of one directory (filtered to the cwd for a custom directory), newest first. */
    virtual std::vector<SessionInfo> list(const std::string& cwd,
                                          const std::optional<std::string>& sessionDir = std::nullopt) = 0;
    /** Sessions of every project directory (or of `sessionDir`), newest first. */
    virtual std::vector<SessionInfo> listAll(const std::optional<std::string>& sessionDir = std::nullopt) = 0;
};
