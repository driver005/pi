module;

#include <cctype>

export module pi.support.path_resolver;

import std;

/**
 * Path normalization for tool arguments (port of core/tools/path-utils.ts and the relevant part
 * of utils/paths.ts): unicode spaces, leading `@`, `~`, file:// URLs, relative-to-cwd resolution.
 */
export class PathResolver {
public:
    explicit PathResolver(std::string homeDirectory)
        : m_home(std::move(homeDirectory)) {}

    /** Unicode spaces to ASCII, strip a leading '@', expand '~', convert file:// URLs. */
    std::string expandPath(const std::string& path) const {
        std::string normalized = normalizeUnicodeSpaces(path);
        if (!normalized.empty() && normalized[0] == '@') {
            normalized.erase(0, 1);
        }
        if (normalized == "~") {
            return m_home;
        }
        if (normalized.rfind("~/", 0) == 0) {
            return (std::filesystem::path(m_home) / normalized.substr(2)).string();
        }
        if (normalized.rfind("file://", 0) == 0) {
            return percentDecode(normalized.substr(7));
        }
        return normalized;
    }

    /** expandPath() then absolute-ize against cwd and lexically normalize (no symlink access). */
    std::string resolveToCwd(const std::string& path, const std::string& cwd) const {
        const std::filesystem::path expanded(expandPath(path));
        std::filesystem::path absolute = expanded.is_absolute() ? expanded : std::filesystem::path(cwd) / expanded;
        std::string normalized = absolute.lexically_normal().string();
        if (normalized.size() > 1 && normalized.back() == '/') {
            normalized.pop_back();
        }
        return normalized;
    }

    /**
     * Alternative spellings to try for a missing file, covering macOS screenshot names:
     * narrow no-break space before AM/PM and curly apostrophes. Excludes `resolved` itself.
     */
    std::vector<std::string> readPathVariants(const std::string& resolved) const {
        std::vector<std::string> variants;
        const std::string narrow = "\xE2\x80\xAF";
        std::string amPm = resolved;
        for (const char* marker : {" AM.", " PM.", " am.", " pm."}) {
            amPm = replaceAll(amPm, marker, narrow + std::string(marker + 1));
        }
        const std::string curly = replaceAll(resolved, "'", "\xE2\x80\x99");
        const std::string both = replaceAll(amPm, "'", "\xE2\x80\x99");
        for (const std::string& candidate : {amPm, curly, both}) {
            if (candidate != resolved && std::find(variants.begin(), variants.end(), candidate) == variants.end()) {
                variants.push_back(candidate);
            }
        }
        return variants;
    }

    /** `path` relative to `base` ('/' separated) when inside it, otherwise unchanged. */
    std::string relativeTo(const std::string& path, const std::string& base) const {
        const std::filesystem::path relative = std::filesystem::path(path).lexically_relative(base);
        const std::string text = relative.generic_string();
        if (relative.empty() || text.rfind("..", 0) == 0) {
            return path;
        }
        return text;
    }

private:
    std::string normalizeUnicodeSpaces(const std::string& text) const {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size();) {
            const auto c = static_cast<unsigned char>(text[i]);
            std::size_t length = 1;
            unsigned int codePoint = c;
            if (c >= 0xF0 && i + 3 < text.size()) {
                length = 4;
            } else if (c >= 0xE0 && i + 2 < text.size()) {
                length = 3;
                codePoint = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) |
                            (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
            } else if (c >= 0xC0 && i + 1 < text.size()) {
                length = 2;
                codePoint = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            }
            const bool unicodeSpace = codePoint == 0x00A0 || (codePoint >= 0x2000 && codePoint <= 0x200A) ||
                                      codePoint == 0x202F || codePoint == 0x205F || codePoint == 0x3000;
            if (unicodeSpace) {
                out.push_back(' ');
            } else {
                out.append(text, i, length);
            }
            i += length;
        }
        return out;
    }

    std::string replaceAll(std::string text, const std::string& from, const std::string& to) const {
        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
        return text;
    }

    std::string percentDecode(const std::string& text) const {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) != 0 &&
                std::isxdigit(static_cast<unsigned char>(text[i + 2])) != 0) {
                out.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            } else {
                out.push_back(text[i]);
            }
        }
        return out;
    }

    std::string m_home;
};
