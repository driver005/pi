export module pi.types.file_lists;

import std;

/** Final sorted file lists: files only read, and files written or edited. */
export struct FileLists {
    std::vector<std::string> readFiles;
    std::vector<std::string> modifiedFiles;
};
