export module pi.types.package_resources;

import std;
export import pi.types.package_resource_entry;

/**
 * The resources the configured packages contribute, in precedence order (project packages before user packages; the first entry
 * of a path wins): skill files, prompt template files and plugin libraries.
 */
export struct PackageResources {
    std::vector<PackageResourceEntry> skills;
    std::vector<PackageResourceEntry> prompts;
    std::vector<PackageResourceEntry> plugins;
};
