export module pi.platform.i_file_lock;

import std;
export import pi.types.result;

/** Cross-process mutual exclusion keyed by a file path (settings.json, auth.json, ...). */
export class IFileLock {
public:
    virtual ~IFileLock() = default;

    /**
     * Runs `action` while holding an exclusive lock for `path`. Blocks until acquired.
     * The lock lives in a sibling "<path>.lock" file so the protected file may be replaced.
     */
    virtual Result<void> withLock(const std::string& path,
                                  const std::function<Result<void>()>& action) = 0;
};
