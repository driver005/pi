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

    /** Percent-encodes everything but the unreserved characters (RFC 3986), for query parameters and form bodies. */
    std::string encode(std::string_view text) const {
        std::string out;
        for (const char c : text) {
            const auto byte = static_cast<unsigned char>(c);
            if (std::isalnum(byte) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
                out.push_back(c);
            } else {
                out += std::format("%{:02X}", byte);
            }
        }
        return out;
    }

    /** Decodes percent-escapes and, in query strings, '+' as a space; malformed escapes stay as written. */
    std::string decode(std::string_view text, bool plusIsSpace = true) const {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '+' && plusIsSpace) {
                out.push_back(' ');
            } else if (text[i] == '%' && i + 2 < text.size() + 0 && std::isxdigit(static_cast<unsigned char>(text[i + 1])) != 0 &&
                       std::isxdigit(static_cast<unsigned char>(text[i + 2])) != 0) {
                out.push_back(static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16)));
                i += 2;
            } else {
                out.push_back(text[i]);
            }
        }
        return out;
    }

    /** The parameters of a query string (`a=1&b=two`), decoded, in order. */
    std::vector<std::pair<std::string, std::string>> parseQuery(std::string_view query) const {
        std::vector<std::pair<std::string, std::string>> out;
        std::size_t start = 0;
        while (start <= query.size()) {
            const std::size_t end = std::min(query.find('&', start), query.size());
            const std::string_view pair = query.substr(start, end - start);
            if (!pair.empty()) {
                const std::size_t equals = pair.find('=');
                out.emplace_back(decode(pair.substr(0, equals)), equals == std::string_view::npos ? std::string() : decode(pair.substr(equals + 1)));
            }
            start = end + 1;
        }
        return out;
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
