export module pi.support.url_parser;

import std;
export import pi.types.parsed_url;
export import pi.types.result;

/**
 * Parses and prints absolute http(s) URLs the way WHATWG URL does for the cases the OAuth code needs: lower-case scheme and
 * host, default port dropped, an empty path becoming "/". Percent-encoding and dot segments are left as written, userinfo is
 * refused. Errors have the code "invalid_url".
 */
export class UrlParser {
public:
    Result<ParsedUrl> parse(const std::string& text) const {
        const std::size_t schemeEnd = text.find("://");
        if (schemeEnd == std::string::npos) {
            return std::unexpected(invalid(text));
        }
        ParsedUrl url;
        url.scheme = lower(text.substr(0, schemeEnd));
        if (url.scheme != "http" && url.scheme != "https") {
            return std::unexpected(invalid(text));
        }
        std::string rest = text.substr(schemeEnd + 3);
        const std::size_t hashAt = rest.find('#');
        if (hashAt != std::string::npos) {
            url.fragment = rest.substr(hashAt + 1);
            rest.resize(hashAt);
        }
        const std::size_t queryAt = rest.find('?');
        if (queryAt != std::string::npos) {
            url.query = rest.substr(queryAt + 1);
            rest.resize(queryAt);
        }
        const std::size_t pathAt = rest.find('/');
        const std::string authority = pathAt == std::string::npos ? rest : rest.substr(0, pathAt);
        url.path = pathAt == std::string::npos ? "/" : rest.substr(pathAt);
        if (authority.empty() || authority.find('@') != std::string::npos) {
            return std::unexpected(invalid(text));
        }
        return withAuthority(std::move(url), authority, text);
    }

    /** `scheme://host[:port]`. */
    std::string origin(const ParsedUrl& url) const {
        return url.scheme + "://" + url.host + (url.port ? ":" + std::to_string(*url.port) : "");
    }

    /** The URL as text; `withFragment` false leaves the fragment out. */
    std::string print(const ParsedUrl& url, bool withFragment = true) const {
        return origin(url) + url.path + (url.query.empty() ? "" : "?" + url.query) +
               (withFragment && !url.fragment.empty() ? "#" + url.fragment : "");
    }

    /** `parse` then `print`: the form servers' URLs are compared and stored in. */
    Result<std::string> normalize(const std::string& text) const {
        auto url = parse(text);
        if (!url) {
            return std::unexpected(url.error());
        }
        return print(*url);
    }

    /** `loopback` hosts may use http for OAuth endpoints. */
    bool loopback(const ParsedUrl& url) const {
        return url.host == "localhost" || url.host == "127.0.0.1" || url.host == "[::1]";
    }

private:
    Result<ParsedUrl> withAuthority(ParsedUrl url, const std::string& authority, const std::string& text) const {
        std::string host = authority;
        std::string port;
        if (authority.starts_with("[")) {
            const std::size_t close = authority.find(']');
            if (close == std::string::npos) {
                return std::unexpected(invalid(text));
            }
            host = authority.substr(0, close + 1);
            if (close + 1 < authority.size()) {
                if (authority[close + 1] != ':') {
                    return std::unexpected(invalid(text));
                }
                port = authority.substr(close + 2);
            }
        } else if (const std::size_t colon = authority.rfind(':'); colon != std::string::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        }
        if (host.empty()) {
            return std::unexpected(invalid(text));
        }
        url.host = lower(host);
        if (!port.empty()) {
            int value = 0;
            const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
            if (parsed.ec != std::errc() || parsed.ptr != port.data() + port.size() || value < 0 || value > 65535) {
                return std::unexpected(invalid(text));
            }
            const int standard = url.scheme == "https" ? 443 : 80;
            if (value != standard) {
                url.port = value;
            }
        }
        return url;
    }

    std::string lower(std::string value) const {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    Error invalid(const std::string& text) const {
        return Error{"invalid_url", "Invalid URL: " + text};
    }
};
