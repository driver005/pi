export module pi.support.bug_report_redactor;

import std;
export import pi.types.json;

/**
 * Removes what could be a credential before a bug report leaves the machine: userinfo and secret-looking query parameters of
 * URLs, and every JSON value stored under a key that names a secret (api key, secret, token, password, credential,
 * authorization, cookie, in any case style). Port of redactUrl and redactJsonValue in core/bug-report.ts.
 */
export class BugReportRedactor {
public:
    /** The text replacing a removed value. */
    std::string redacted() const {
        return "<redacted>";
    }

    bool isSensitiveKey(const std::string& key) const {
        // camelCase to snake_case, then look for a secret word delimited by the start, the end, '-' or '_'.
        std::string spaced;
        for (std::size_t i = 0; i < key.size(); ++i) {
            if (i > 0 && std::isupper(static_cast<unsigned char>(key[i])) != 0 && (std::islower(static_cast<unsigned char>(key[i - 1])) != 0 || std::isdigit(static_cast<unsigned char>(key[i - 1])) != 0)) {
                spaced.push_back('_');
            }
            spaced.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(key[i]))));
        }
        for (const std::string word : {"apikey", "api_key", "api-key", "secret", "token", "password", "passwd", "credential", "authorization", "cookie"}) {
            std::size_t at = spaced.find(word);
            while (at != std::string::npos) {
                const bool startOk = at == 0 || spaced[at - 1] == '-' || spaced[at - 1] == '_';
                const std::size_t end = at + word.size();
                const bool endOk = end == spaced.size() || spaced[end] == '-' || spaced[end] == '_';
                if (startOk && endOk) {
                    return true;
                }
                at = spaced.find(word, at + 1);
            }
        }
        return false;
    }

    /** A URL without credentials or secret query values; text that is not a URL, and a URL with nothing to remove, unchanged. */
    std::string redactUrl(const std::string& value) const {
        const std::size_t colon = value.find(':');
        if (colon != std::string::npos && colon > 0 && isScheme(value.substr(0, colon)) && value.compare(colon + 1, 3, "://") != 0) {
            const std::string rest = value.substr(colon + 1);
            if (isScheme(rest.substr(0, rest.find(':'))) && rest.find("://") != std::string::npos) {
                return value.substr(0, colon + 1) + redactUrl(rest);
            }
        }
        const std::size_t schemeEnd = value.find("://");
        if (schemeEnd == std::string::npos || schemeEnd == 0 || !isScheme(value.substr(0, schemeEnd))) {
            return value;
        }
        const std::string scheme = value.substr(0, schemeEnd + 3);
        std::string rest = value.substr(schemeEnd + 3);
        std::string fragment;
        if (const std::size_t hash = rest.find('#'); hash != std::string::npos) {
            fragment = rest.substr(hash);
            rest.resize(hash);
        }
        std::string query;
        if (const std::size_t question = rest.find('?'); question != std::string::npos) {
            query = rest.substr(question + 1);
            rest.resize(question);
        }
        const std::size_t slash = rest.find('/');
        std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
        const std::string path = slash == std::string::npos ? "" : rest.substr(slash);
        bool changed = false;
        if (const std::size_t at = authority.rfind('@'); at != std::string::npos) {
            authority = authority.substr(at + 1);
            changed = true;
        }
        const std::string newQuery = redactQuery(query, changed);
        if (!changed) {
            return value;
        }
        return scheme + authority + path + (query.empty() ? "" : "?" + newQuery) + fragment;
    }

    /** A copy of `value` with secrets removed. */
    Json redactJson(const Json& value) const {
        if (value.is_object()) {
            Json out = Json::object();
            for (const auto& entry : value.items()) {
                out[entry.key()] = !entry.value().is_null() && isSensitiveKey(entry.key()) ? Json(redacted()) : redactJson(entry.value());
            }
            return out;
        }
        if (value.is_array()) {
            Json out = Json::array();
            for (const Json& item : value) {
                out.push_back(redactJson(item));
            }
            return out;
        }
        return value.is_string() ? Json(redactUrl(value.get<std::string>())) : value;
    }

private:
    bool isScheme(const std::string& text) const {
        if (text.empty() || std::isalpha(static_cast<unsigned char>(text[0])) == 0) {
            return false;
        }
        return std::ranges::all_of(text, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '.' || c == '-'; });
    }

    std::string redactQuery(const std::string& query, bool& changed) const {
        std::string out;
        std::size_t begin = 0;
        while (begin <= query.size() && !query.empty()) {
            const std::size_t end = query.find('&', begin);
            const std::string pair = query.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            const std::size_t equals = pair.find('=');
            const std::string key = pair.substr(0, equals);
            if (!out.empty()) {
                out += "&";
            }
            if (isSensitiveKey(decode(key))) {
                out += key + "=" + encoded();
                changed = true;
            } else {
                out += pair;
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
        return out;
    }

    std::string encoded() const {
        return "%3Credacted%3E";
    }

    std::string decode(const std::string& text) const {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '%' && i + 2 < text.size() + 0 && std::isxdigit(static_cast<unsigned char>(text[i + 1])) != 0 && std::isxdigit(static_cast<unsigned char>(text[i + 2])) != 0) {
                out.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            } else {
                out.push_back(text[i] == '+' ? ' ' : text[i]);
            }
        }
        return out;
    }
};
