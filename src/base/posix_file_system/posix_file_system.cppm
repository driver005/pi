module;

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

export module pi.base.posix_file_system;

import std;
export import pi.platform.i_file_system;

/** IFileSystem over POSIX calls; errors carry the errno name as code. */
export class PosixFileSystem : public IFileSystem {
public:
    Result<std::string> readFile(const std::string& path) override {
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return std::unexpected(failure(errno, "open", path));
        }
        std::string content;
        char buffer[65536];
        while (true) {
            const ssize_t count = read(fd, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                const int code = errno;
                close(fd);
                return std::unexpected(failure(code, "read", path));
            }
            if (count == 0) {
                break;
            }
            content.append(buffer, static_cast<std::size_t>(count));
        }
        close(fd);
        return content;
    }

    Result<void> writeFile(const std::string& path, const std::string& content) override {
        return writeWithFlags(path, content, O_WRONLY | O_CREAT | O_TRUNC);
    }

    Result<void> appendFile(const std::string& path, const std::string& content) override {
        return writeWithFlags(path, content, O_WRONLY | O_CREAT | O_APPEND);
    }

    Result<void> createDirectories(const std::string& path) override {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error) {
            return std::unexpected(failure(error.value(), "mkdir", path));
        }
        return {};
    }

    Result<void> removeFile(const std::string& path) override {
        if (unlink(path.c_str()) != 0) {
            return std::unexpected(failure(errno, "unlink", path));
        }
        return {};
    }

    Result<void> renameFile(const std::string& from, const std::string& to) override {
        if (rename(from.c_str(), to.c_str()) != 0) {
            return std::unexpected(failure(errno, "rename", from));
        }
        return {};
    }

    bool exists(const std::string& path) override {
        return access(path.c_str(), F_OK) == 0;
    }

    bool isReadable(const std::string& path) override {
        return access(path.c_str(), R_OK) == 0;
    }

    bool isWritable(const std::string& path) override {
        return access(path.c_str(), W_OK) == 0;
    }

    Result<FileStat> stat(const std::string& path) override {
        struct stat info {};
        if (::stat(path.c_str(), &info) != 0) {
            return std::unexpected(failure(errno, "stat", path));
        }
        FileStat result;
        result.isFile = S_ISREG(info.st_mode);
        result.isDirectory = S_ISDIR(info.st_mode);
        result.size = static_cast<std::uint64_t>(info.st_size);
        result.mtimeMs = static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1000 + info.st_mtim.tv_nsec / 1000000;
        return result;
    }

    Result<std::vector<std::string>> listDirectory(const std::string& path) override {
        std::error_code error;
        std::filesystem::directory_iterator iterator(path, error);
        if (error) {
            return std::unexpected(failure(error.value(), "scandir", path));
        }
        std::vector<std::string> names;
        for (const auto& entry : iterator) {
            names.push_back(entry.path().filename().string());
        }
        return names;
    }

    std::string realPath(const std::string& path) override {
        char buffer[PATH_MAX];
        if (realpath(path.c_str(), buffer) == nullptr) {
            return path;
        }
        return buffer;
    }

    std::string homeDirectory() override {
        if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            return home;
        }
        if (const passwd* entry = getpwuid(getuid()); entry != nullptr && entry->pw_dir != nullptr) {
            return entry->pw_dir;
        }
        return "/";
    }

private:
    Result<void> writeWithFlags(const std::string& path, const std::string& content, int flags) {
        const int fd = open(path.c_str(), flags | O_CLOEXEC, 0644);
        if (fd < 0) {
            return std::unexpected(failure(errno, "open", path));
        }
        std::size_t written = 0;
        while (written < content.size()) {
            const ssize_t count = write(fd, content.data() + written, content.size() - written);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                const int code = errno;
                close(fd);
                return std::unexpected(failure(code, "write", path));
            }
            written += static_cast<std::size_t>(count);
        }
        close(fd);
        return {};
    }

    std::string errnoName(int code) const {
        switch (code) {
            case ENOENT: return "ENOENT";
            case EACCES: return "EACCES";
            case EPERM: return "EPERM";
            case EISDIR: return "EISDIR";
            case ENOTDIR: return "ENOTDIR";
            case EEXIST: return "EEXIST";
            case ENOSPC: return "ENOSPC";
            case ENOTEMPTY: return "ENOTEMPTY";
            case EROFS: return "EROFS";
            case EMFILE: return "EMFILE";
            case ENAMETOOLONG: return "ENAMETOOLONG";
            default: return "EIO";
        }
    }

    Error failure(int code, const std::string& operation, const std::string& path) const {
        std::string reason = std::strerror(code);
        if (!reason.empty()) {
            reason[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(reason[0])));
        }
        return Error{errnoName(code), errnoName(code) + ": " + reason + ", " + operation + " '" + path + "'"};
    }
};
