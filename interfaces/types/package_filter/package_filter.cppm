export module pi.types.package_filter;

import std;

/**
 * The object form of a `packages` entry in settings narrows what a package contributes: for each resource type an absent list
 * loads everything the package offers, `[]` nothing, and entries are glob includes, `!pattern` excludes, `+path` exact
 * force-includes and `-path` exact force-excludes. `autoload: false` makes the entry a delta over the same package of another
 * scope. Resource types are `skills`, `prompts` and `plugins` (the native counterpart of TypeScript extensions).
 */
export struct PackageFilter {
    std::optional<bool> autoload;
    std::optional<std::vector<std::string>> skills;
    std::optional<std::vector<std::string>> prompts;
    std::optional<std::vector<std::string>> plugins;
};
