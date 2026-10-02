export module pi.support.prompt_template_expander;

import std;
export import pi.types.prompt_template;

/**
 * Argument parsing and placeholder substitution for prompt templates. Supported placeholders:
 * $1.. positional, $@ and $ARGUMENTS all, ${N:-default}, ${@:-default}, ${ARGUMENTS:-default},
 * ${@:N} from the Nth, ${@:N:L} L arguments from the Nth. Substituted values are never rescanned.
 * Port of parseCommandArgs / substituteArgs / expandPromptTemplate in core/prompt-templates.ts.
 */
export class PromptTemplateExpander {
public:
    /** Bash-style splitting: whitespace separates, single or double quotes group. */
    std::vector<std::string> parseArgs(const std::string& text) const;

    std::string substitute(const std::string& content, const std::vector<std::string>& args) const;

    /** "/name rest" expands the template called name; any other text is returned unchanged. */
    std::string expand(const std::string& text, const std::vector<PromptTemplate>& templates) const;

private:
    std::string join(const std::vector<std::string>& args, std::size_t from, std::size_t count) const;
    bool isDigits(const std::string& text) const;
    /** Tries to read a placeholder at text[pos] == '$'; returns the replacement and consumed length. */
    std::optional<std::pair<std::string, std::size_t>> placeholder(const std::string& text, std::size_t pos,
                                                                   const std::vector<std::string>& args) const;
};

bool PromptTemplateExpander::isDigits(const std::string& text) const {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::vector<std::string> PromptTemplateExpander::parseArgs(const std::string& text) const {
    std::vector<std::string> args;
    std::string current;
    char quote = 0;
    for (const char c : text) {
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            } else {
                current.push_back(c);
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (!current.empty()) {
                args.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        args.push_back(std::move(current));
    }
    return args;
}

std::string PromptTemplateExpander::join(const std::vector<std::string>& args, std::size_t from,
                                         std::size_t count) const {
    std::string out;
    for (std::size_t i = from; i < args.size() && i - from < count; ++i) {
        out += (i > from ? " " : "") + args[i];
    }
    return out;
}

std::optional<std::pair<std::string, std::size_t>> PromptTemplateExpander::placeholder(
    const std::string& text, std::size_t pos, const std::vector<std::string>& args) const {
    const std::string all = join(args, 0, args.size());
    const std::size_t rest = text.size() - pos;
    if (rest >= 3 && text[pos + 1] == '{') {
        const std::size_t close = text.find('}', pos + 2);
        if (close != std::string::npos) {
            const std::string inner = text.substr(pos + 2, close - pos - 2);
            const std::size_t defaultMark = inner.find(":-");
            if (defaultMark != std::string::npos) {
                const std::string target = inner.substr(0, defaultMark);
                const std::string fallback = inner.substr(defaultMark + 2);
                if (isDigits(target) || target == "ARGUMENTS" || target == "@") {
                    std::string value;
                    if (target == "ARGUMENTS" || target == "@") {
                        value = all;
                    } else {
                        const std::size_t index = std::stoul(target);
                        value = index >= 1 && index <= args.size() ? args[index - 1] : "";
                    }
                    return std::make_pair(value.empty() ? fallback : value, close + 1 - pos);
                }
            } else if (inner.rfind("@:", 0) == 0) {
                const std::string spec = inner.substr(2);
                const std::size_t colon = spec.find(':');
                const std::string startText = spec.substr(0, colon);
                const std::string lengthText = colon == std::string::npos ? "" : spec.substr(colon + 1);
                if (isDigits(startText) && (colon == std::string::npos || isDigits(lengthText))) {
                    const std::size_t start = std::max<std::size_t>(std::stoul(startText), 1) - 1;
                    const std::size_t count = colon == std::string::npos ? args.size() : std::stoul(lengthText);
                    return std::make_pair(join(args, start, count), close + 1 - pos);
                }
            }
        }
    }
    if (rest >= 2) {
        if (text.compare(pos + 1, 9, "ARGUMENTS") == 0) {
            return std::make_pair(all, 10);
        }
        if (text[pos + 1] == '@') {
            return std::make_pair(all, 2);
        }
        if (text[pos + 1] >= '0' && text[pos + 1] <= '9') {
            std::size_t end = pos + 1;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
                ++end;
            }
            const std::size_t index = std::stoul(text.substr(pos + 1, end - pos - 1));
            return std::make_pair(index >= 1 && index <= args.size() ? args[index - 1] : "", end - pos);
        }
    }
    return std::nullopt;
}

std::string PromptTemplateExpander::substitute(const std::string& content,
                                               const std::vector<std::string>& args) const {
    std::string out;
    std::size_t i = 0;
    while (i < content.size()) {
        if (content[i] == '$') {
            if (const auto replaced = placeholder(content, i, args)) {
                out += replaced->first;
                i += replaced->second;
                continue;
            }
        }
        out.push_back(content[i++]);
    }
    return out;
}

std::string PromptTemplateExpander::expand(const std::string& text,
                                           const std::vector<PromptTemplate>& templates) const {
    if (text.empty() || text[0] != '/') {
        return text;
    }
    std::size_t nameEnd = 1;
    while (nameEnd < text.size() && std::isspace(static_cast<unsigned char>(text[nameEnd])) == 0) {
        ++nameEnd;
    }
    if (nameEnd == 1) {
        return text;
    }
    const std::string name = text.substr(1, nameEnd - 1);
    std::string rest;
    if (nameEnd < text.size()) {
        std::size_t argsStart = nameEnd;
        while (argsStart < text.size() && std::isspace(static_cast<unsigned char>(text[argsStart])) != 0) {
            ++argsStart;
        }
        rest = text.substr(argsStart);
    }
    for (const auto& candidate : templates) {
        if (candidate.name == name) {
            return substitute(candidate.content, parseArgs(rest));
        }
    }
    return text;
}
