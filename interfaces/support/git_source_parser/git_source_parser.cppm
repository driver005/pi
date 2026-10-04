export module pi.support.git_source_parser;

import std;
export import pi.types.package_source;

/**
 * Reads a git package source. With the `git:` prefix every shorthand is accepted: `host/user/repo`, `git@host:user/repo`,
 * protocol URLs and `user/repo` (GitHub); without it only explicit `https://`, `http://`, `ssh://` and `git://` URLs are. A ref
 * follows the repository after `@` (`github.com/user/repo@v1`); `.git` suffixes are dropped. Hosts and paths that could leave
 * the install directory are refused. Port of utils/git.ts without the hosted-git-info shorthands of other hosts.
 */
export class GitSourceParser {
public:
    std::optional<PackageSource> parse(const std::string& source) const {
        const std::string trimmed = trim(source);
        const bool prefixed = trimmed.starts_with("git:");
        const std::string url = prefixed ? trim(trimmed.substr(4)) : trimmed;
        if (!prefixed && !isProtocolUrl(url)) {
            return std::nullopt;
        }
        const auto [repoWithoutRef, ref] = splitRef(url);
        std::string repo = repoWithoutRef;
        std::string host;
        std::string path;
        if (const auto scp = scpLike(repoWithoutRef)) {
            host = scp->first;
            path = scp->second;
        } else if (isProtocolUrl(repoWithoutRef)) {
            const auto parts = splitUrl(repoWithoutRef);
            if (!parts) {
                return std::nullopt;
            }
            host = parts->first;
            path = parts->second;
        } else {
            const std::size_t slash = repoWithoutRef.find('/');
            if (slash == std::string::npos) {
                return std::nullopt;
            }
            host = repoWithoutRef.substr(0, slash);
            path = repoWithoutRef.substr(slash + 1);
            if (host.find('.') == std::string::npos && host != "localhost") {
                // `user/repo` is a GitHub repository.
                path = repoWithoutRef;
                host = "github.com";
                repo = "https://github.com/" + repoWithoutRef;
            } else {
                repo = "https://" + repoWithoutRef;
            }
        }
        return build(repo, host, path, ref);
    }

private:
    std::optional<PackageSource> build(const std::string& repo, const std::string& host, std::string path, const std::optional<std::string>& ref) const {
        if (path.starts_with("/")) {
            return std::nullopt;
        }
        if (path.ends_with(".git")) {
            path.resize(path.size() - 4);
        }
        const std::size_t first = path.find_first_not_of('/');
        path = first == std::string::npos ? "" : path.substr(first);
        if (host.empty() || path.empty() || std::ranges::count(path, '/') < 1 || unsafe(host, false) || unsafe(path, true)) {
            return std::nullopt;
        }
        PackageSource out;
        out.type = "git";
        out.repo = repo;
        out.host = host;
        out.path = path;
        out.ref = ref;
        out.pinned = ref.has_value();
        return out;
    }

    /** `{repository, ref}`: a ref is what follows the first `@` of the path. */
    std::pair<std::string, std::optional<std::string>> splitRef(const std::string& url) const {
        if (const auto scp = scpLike(url)) {
            const std::size_t at = scp->second.find('@');
            if (at == std::string::npos || at == 0 || at + 1 == scp->second.size()) {
                return {url, std::nullopt};
            }
            return {"git@" + scp->first + ":" + scp->second.substr(0, at), scp->second.substr(at + 1)};
        }
        const std::size_t scheme = url.find("://");
        const std::size_t pathStart = scheme == std::string::npos ? url.find('/') : url.find('/', scheme + 3);
        if (pathStart == std::string::npos) {
            return {url, std::nullopt};
        }
        const std::string prefix = url.substr(0, pathStart + 1);
        std::string rest = url.substr(pathStart + 1);
        if (scheme != std::string::npos) {
            const std::size_t skip = rest.find_first_not_of('/');
            rest = skip == std::string::npos ? "" : rest.substr(skip);
        }
        const std::size_t at = rest.find('@');
        if (at == std::string::npos || at == 0 || at + 1 == rest.size()) {
            return {url, std::nullopt};
        }
        return {prefix + rest.substr(0, at), rest.substr(at + 1)};
    }

    /** `git@host:path` as `{host, path}`. */
    std::optional<std::pair<std::string, std::string>> scpLike(const std::string& text) const {
        if (!text.starts_with("git@")) {
            return std::nullopt;
        }
        const std::size_t colon = text.find(':', 4);
        if (colon == std::string::npos || colon == 4 || colon + 1 == text.size()) {
            return std::nullopt;
        }
        return std::make_pair(text.substr(4, colon - 4), text.substr(colon + 1));
    }

    /** `{hostname, path}` of a protocol URL: no user, port, query or fragment in the host and path. */
    std::optional<std::pair<std::string, std::string>> splitUrl(const std::string& url) const {
        const std::size_t start = url.find("://") + 3;
        const std::size_t slash = url.find('/', start);
        std::string authority = url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        std::string path = slash == std::string::npos ? "" : url.substr(slash + 1);
        if (const std::size_t at = authority.rfind('@'); at != std::string::npos) {
            authority = authority.substr(at + 1);
        }
        if (const std::size_t colon = authority.find(':'); colon != std::string::npos) {
            authority.resize(colon);
        }
        for (const char stop : {'?', '#'}) {
            if (const std::size_t at = path.find(stop); at != std::string::npos) {
                path.resize(at);
            }
        }
        const std::size_t skip = path.find_first_not_of('/');
        path = skip == std::string::npos ? "" : path.substr(skip);
        if (authority.empty()) {
            return std::nullopt;
        }
        std::string lower = authority;
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return std::make_pair(lower, path);
    }

    bool isProtocolUrl(const std::string& text) const {
        std::string lower = text.substr(0, 8);
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.starts_with("https://") || lower.starts_with("http://") || lower.starts_with("ssh://") || lower.starts_with("git://");
    }

    /** A host or path that is or decodes to something outside the install directory. */
    bool unsafe(const std::string& value, bool allowSlash) const {
        const std::optional<std::string> decoded = decode(value);
        if (!decoded) {
            return true;
        }
        for (const std::string& candidate : {value, *decoded}) {
            if (candidate.find('\0') != std::string::npos || candidate.find('\\') != std::string::npos || candidate.starts_with("/")) {
                return true;
            }
            if (!allowSlash && candidate.find('/') != std::string::npos) {
                return true;
            }
            std::size_t begin = 0;
            while (begin <= candidate.size()) {
                const std::size_t end = candidate.find('/', begin);
                if (candidate.substr(begin, end == std::string::npos ? std::string::npos : end - begin) == "..") {
                    return true;
                }
                if (end == std::string::npos) {
                    break;
                }
                begin = end + 1;
            }
        }
        return false;
    }

    std::optional<std::string> decode(const std::string& text) const {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '%') {
                out.push_back(text[i]);
                continue;
            }
            if (i + 3 > text.size()) {
                return std::nullopt;
            }
            int value = 0;
            const auto [end, error] = std::from_chars(text.data() + i + 1, text.data() + i + 3, value, 16);
            if (error != std::errc() || end != text.data() + i + 3) {
                return std::nullopt;
            }
            out.push_back(static_cast<char>(value));
            i += 2;
        }
        return out;
    }

    std::string trim(const std::string& text) const {
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }
};
