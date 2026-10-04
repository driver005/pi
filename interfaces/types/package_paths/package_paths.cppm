export module pi.types.package_paths;

import std;

/** The files the configured packages contribute that are switched on, plus what went wrong with packages that could not load. */
export struct PackagePaths {
    std::vector<std::string> skills;
    std::vector<std::string> prompts;
    std::vector<std::string> plugins;
    std::vector<std::string> warnings;
};
