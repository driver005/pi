module;

#include <cstdint>

export module pi.types.semver_version;

import std;

/** A parsed semantic version: major, minor and patch, and the dot-separated prerelease identifiers (empty for a release). */
export struct SemverVersion {
    std::array<std::uint64_t, 3> numbers{};
    std::vector<std::string> prerelease;
};
