export module pi.types.package_resource_entry;

import std;

/** One file a package offers (a skill file, a prompt template or a plugin library) and whether the package's filters leave it on. */
export struct PackageResourceEntry {
    std::string path;
    bool enabled = true;
};
