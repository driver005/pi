module;

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

export module pi.base.posix_file_lock;

import std;
export import pi.platform.i_file_lock;

/** IFileLock using flock(2) on "<path>.lock". */
export class PosixFileLock : public IFileLock {
public:
    Result<void> withLock(const std::string& path,
                          const std::function<Result<void>()>& action) override;
};

Result<void> PosixFileLock::withLock(const std::string& path,
                                     const std::function<Result<void>()>& action) {
    const std::string lockPath = path + ".lock";
    const int fd = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        return std::unexpected(Error{"lock_open", lockPath + ": " + std::strerror(errno)});
    }
    int rc = flock(fd, LOCK_EX);
    while (rc != 0 && errno == EINTR) {
        rc = flock(fd, LOCK_EX);
    }
    if (rc != 0) {
        const std::string reason = std::strerror(errno);
        close(fd);
        return std::unexpected(Error{"lock_acquire", lockPath + ": " + reason});
    }
    Result<void> outcome = action();
    flock(fd, LOCK_UN);
    close(fd);
    return outcome;
}
