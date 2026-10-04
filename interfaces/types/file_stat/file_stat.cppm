module;

#include <cstdint>

export module pi.types.file_stat;

import std;

/** Result of stat() (symlinks followed). */
export struct FileStat {
    bool isFile = false;
    bool isDirectory = false;
    std::uint64_t size = 0;
    std::int64_t mtimeMs = 0;
};
