export module pi.types.zip_entry;

import std;

/** One file of a ZIP archive. */
export struct ZipEntry {
    std::string name;
    std::string data;
};
