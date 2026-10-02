export module pi.support.header_merger;

import std;
export import pi.types.http_headers;

/** Header list helpers: case-insensitive lookup and default/override merging. */
export class HeaderMerger {
public:
    using Override = std::pair<std::string, std::optional<std::string>>;

    /** Value of the first header with this name (any case), or nullopt. */
    std::optional<std::string> find(const HttpHeaders& headers, const std::string& name) const;

    /** Sets or replaces a header, keeping the position of an existing one. */
    void set(HttpHeaders& headers, const std::string& name, const std::string& value) const;

    /** Applies overrides on top of defaults; a nullopt override value removes the header. */
    HttpHeaders merge(const HttpHeaders& defaults, const std::vector<Override>& overrides) const;

    /** Same with a plain map of overrides (model headers). */
    HttpHeaders merge(const HttpHeaders& defaults,
                      const std::map<std::string, std::string>& overrides) const;

private:
    std::string lower(const std::string& text) const;
    bool equal(const std::string& left, const std::string& right) const;
};

std::string HeaderMerger::lower(const std::string& text) const {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool HeaderMerger::equal(const std::string& left, const std::string& right) const {
    return lower(left) == lower(right);
}

std::optional<std::string> HeaderMerger::find(const HttpHeaders& headers,
                                              const std::string& name) const {
    for (const auto& header : headers) {
        if (equal(header.first, name)) {
            return header.second;
        }
    }
    return std::nullopt;
}

void HeaderMerger::set(HttpHeaders& headers, const std::string& name,
                       const std::string& value) const {
    for (auto& header : headers) {
        if (equal(header.first, name)) {
            header.second = value;
            return;
        }
    }
    headers.emplace_back(name, value);
}

HttpHeaders HeaderMerger::merge(const HttpHeaders& defaults,
                                const std::vector<Override>& overrides) const {
    HttpHeaders result = defaults;
    for (const auto& entry : overrides) {
        if (entry.second) {
            set(result, entry.first, *entry.second);
            continue;
        }
        std::erase_if(result, [&](const auto& header) { return equal(header.first, entry.first); });
    }
    return result;
}

HttpHeaders HeaderMerger::merge(const HttpHeaders& defaults,
                                const std::map<std::string, std::string>& overrides) const {
    std::vector<Override> list;
    for (const auto& entry : overrides) {
        list.emplace_back(entry.first, entry.second);
    }
    return merge(defaults, list);
}
