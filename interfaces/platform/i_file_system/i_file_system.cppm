export module pi.platform.i_file_system;

import std;
export import pi.types.file_stat;
export import pi.types.result;

/**
 * Local file access. Errors carry an errno name as code ("ENOENT", "EACCES", "EISDIR", ...) and
 * the system message as text, so tools can report them like Node does.
 */
export class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    virtual Result<std::string> readFile(const std::string& path) = 0;
    virtual Result<void> writeFile(const std::string& path, const std::string& content) = 0;
    virtual Result<void> appendFile(const std::string& path, const std::string& content) = 0;
    virtual Result<void> createDirectories(const std::string& path) = 0;
    virtual Result<void> removeFile(const std::string& path) = 0;
    virtual Result<void> renameFile(const std::string& from, const std::string& to) = 0;
    virtual bool exists(const std::string& path) = 0;
    virtual bool isReadable(const std::string& path) = 0;
    virtual bool isWritable(const std::string& path) = 0;
    virtual Result<FileStat> stat(const std::string& path) = 0;
    /** Entry names (not paths), unsorted, without "." and "..". */
    virtual Result<std::vector<std::string>> listDirectory(const std::string& path) = 0;
    /** Canonical path with symlinks resolved; the input unchanged when resolution fails. */
    virtual std::string realPath(const std::string& path) = 0;
    virtual std::string homeDirectory() = 0;
};
