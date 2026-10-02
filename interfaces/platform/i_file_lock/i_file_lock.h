#pragma once

#include <functional>
#include <string>

#include "interfaces/types/result/result.h"

/** Cross-process mutual exclusion keyed by a file path (settings.json, auth.json, ...). */
class IFileLock {
public:
    virtual ~IFileLock() = default;

    /**
     * Runs `action` while holding an exclusive lock for `path`. Blocks until acquired.
     * The lock lives in a sibling "<path>.lock" file so the protected file may be replaced.
     */
    virtual Result<void> withLock(const std::string& path,
                                  const std::function<Result<void>()>& action) = 0;
};
