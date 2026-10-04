export module pi.types.sqlite_options;

import std;

/** Connection settings of a durable SQLite file. */
export struct SqliteOptions {
    /** WAL auto-checkpoint threshold in pages; 0 disables it. */
    int walAutoCheckpointPages = 1000;
    /** How long SQLite waits for a competing file lock. */
    int busyTimeoutMs = 5000;
};
