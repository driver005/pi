export module pi.types.file_operations;

import std;

/** Paths touched by tool calls, split by how the file was used. */
export struct FileOperations {
    std::set<std::string> read;
    std::set<std::string> written;
    std::set<std::string> edited;
};
