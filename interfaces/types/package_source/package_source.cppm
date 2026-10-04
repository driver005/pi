export module pi.types.package_source;

import std;

/** Where a package comes from: a git repository, a local directory or an npm package. */
export struct PackageSource {
    /** "git", "local" or "npm". */
    std::string type = "local";
    /** git: the clone URL without the ref. */
    std::string repo;
    /** git: the host (github.com) and the repository path (user/repo). */
    std::string host;
    std::string path;
    /** git: the branch, tag or commit; a pinned package is not moved by updates. */
    std::optional<std::string> ref;
    bool pinned = false;
    /** local: the path as written. */
    std::string localPath;
    /** npm: the specification after `npm:`, the package name in it and the version part (`1.2.3`, `^1.2`, `latest`), if any. */
    std::string npmSpec;
    std::string npmName;
    std::optional<std::string> npmVersion;
};
