export module pi.types.session_start_mode;

/** Which session a process starts in. */
export enum class SessionStartMode {
    /** A new persisted session. */
    New,
    /** The most recent session of the cwd, or a new one when there is none. */
    Continue,
    /** The session file or id named by the session reference. */
    Open,
    /** A session that is never written to disk. */
    InMemory
};
