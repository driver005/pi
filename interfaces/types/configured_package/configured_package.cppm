export module pi.types.configured_package;

import std;
export import pi.types.package_filter;

/** One `packages` entry of the global or project settings. */
export struct ConfiguredPackage {
    std::string source;
    /** "user" or "project". */
    std::string scope;
    /** The entry is in object form and carries a filter. */
    bool filtered = false;
    PackageFilter filter;
    /** Where the package is installed (a local package: its directory); absent when it is not installed. */
    std::optional<std::string> installedPath;
};
