#pragma once

#include <functional>
#include <string>

#include "interfaces/platform/i_file_lock/i_file_lock.h"

/** IFileLock using flock(2) on "<path>.lock". */
class PosixFileLock : public IFileLock {
public:
    Result<void> withLock(const std::string& path,
                          const std::function<Result<void>()>& action) override;
};
