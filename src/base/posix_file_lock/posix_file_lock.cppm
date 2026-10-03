module;

#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

export module pi.base.posix_file_lock;

import std;
export import pi.platform.i_file_lock;

/**
 * IFileLock using the lock-directory protocol of Node's proper-lockfile, so C++ and TypeScript
 * processes sharing an auth.json or settings.json exclude each other: the lock is the directory
 * "<path>.lock"; it is stale once its mtime is older than 30s; the holder refreshes the mtime
 * every 15s while the action runs.
 */
export class PosixFileLock : public IFileLock {
public:
    Result<void> withLock(const std::string& path, const std::function<Result<void>()>& action) override {
        const std::string lockPath = path + ".lock";
        if (auto acquired = acquire(lockPath); !acquired) {
            return acquired;
        }
        std::mutex mutex;
        std::condition_variable wake;
        bool done = false;
        std::thread keepAlive([&]() {
            std::unique_lock<std::mutex> lock(mutex);
            while (!wake.wait_for(lock, std::chrono::milliseconds(StaleMs / 2), [&]() { return done; })) {
                touch(lockPath);
            }
        });
        Result<void> outcome = action();
        {
            const std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        wake.notify_all();
        keepAlive.join();
        rmdir(lockPath.c_str());
        return outcome;
    }

private:
    static constexpr std::int64_t StaleMs = 30000;

    Result<void> acquire(const std::string& lockPath) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(StaleMs);
        std::mt19937 random{std::random_device{}()};
        int retry = 0;
        while (true) {
            if (mkdir(lockPath.c_str(), 0700) == 0) {
                return {};
            }
            if (errno != EEXIST) {
                return std::unexpected(Error{"lock_open", lockPath + ": " + std::strerror(errno)});
            }
            if (isStale(lockPath)) {
                rmdir(lockPath.c_str());
                continue;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::unexpected(Error{"lock_acquire", "Lock file is already being held: " + lockPath});
            }
            const double base = std::min(10.0 * std::pow(2.0, retry), 1000.0);
            ++retry;
            const double jitter = std::uniform_real_distribution<double>(1.0, 2.0)(random);
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(base * jitter)));
        }
    }

    bool isStale(const std::string& lockPath) const {
        struct stat info {};
        if (stat(lockPath.c_str(), &info) != 0) {
            return false;
        }
        const std::int64_t modifiedMs = static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1000 +
                                        info.st_mtim.tv_nsec / 1000000;
        const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
        return modifiedMs < nowMs - StaleMs;
    }

    void touch(const std::string& lockPath) const {
        utimes(lockPath.c_str(), nullptr);
    }
};
