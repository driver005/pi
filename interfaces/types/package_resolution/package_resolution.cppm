export module pi.types.package_resolution;

import std;
export import pi.types.package_resources;

/** The resources of all configured packages and what went wrong with the packages that could not contribute. */
export struct PackageResolution {
    PackageResources resources;
    std::vector<std::string> warnings;
};
