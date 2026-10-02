export module pi.support.skill_command_expander;

import std;
export import pi.platform.i_file_system;
export import pi.support.frontmatter_parser;
export import pi.types.parsed_skill_block;
export import pi.types.result;
export import pi.types.skill;

/**
 * Expands "/skill:name args" into the skill's content wrapped in a <skill> block, and reads such
 * blocks back from user messages. Port of AgentSession._expandSkillCommand and parseSkillBlock.
 */
export class SkillCommandExpander {
public:
    explicit SkillCommandExpander(IFileSystem& files);

    /**
     * The text unchanged when it is not a skill command or the skill is unknown; the expanded
     * block otherwise. Error when the skill file cannot be read (callers keep the original text).
     */
    Result<std::string> expand(const std::string& text, const std::vector<Skill>& skills) const;

    std::optional<ParsedSkillBlock> parse(const std::string& text) const;

private:
    std::string trim(const std::string& text) const;
    std::string body(const std::string& content) const;
    std::optional<ParsedSkillBlock> parseTail(const std::string& name, const std::string& location,
                                              const std::string& rest) const;

    IFileSystem& m_files;
    FrontmatterParser m_frontmatter;
};

SkillCommandExpander::SkillCommandExpander(IFileSystem& files) : m_files(files) {}

std::string SkillCommandExpander::trim(const std::string& text) const {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string SkillCommandExpander::body(const std::string& content) const {
    const auto document = m_frontmatter.parse(content);
    return trim(document ? document->body : content);
}

Result<std::string> SkillCommandExpander::expand(const std::string& text,
                                                 const std::vector<Skill>& skills) const {
    if (!text.starts_with("/skill:")) {
        return text;
    }
    const auto space = text.find(' ');
    const std::string name = space == std::string::npos ? text.substr(7) : text.substr(7, space - 7);
    const std::string args = space == std::string::npos ? "" : trim(text.substr(space + 1));
    const auto skill = std::ranges::find_if(skills, [&](const Skill& s) { return s.name == name; });
    if (skill == skills.end()) {
        return text;
    }
    const auto content = m_files.readFile(skill->filePath);
    if (!content) {
        return std::unexpected(content.error());
    }
    std::string block = "<skill name=\"" + skill->name + "\" location=\"" + skill->filePath + "\">\nReferences are relative to " +
                        skill->baseDir + ".\n\n" + body(*content) + "\n</skill>";
    return args.empty() ? block : block + "\n\n" + args;
}

std::optional<ParsedSkillBlock> SkillCommandExpander::parseTail(const std::string& name,
                                                                const std::string& location,
                                                                const std::string& rest) const {
    // The content ends at the first "\n</skill>" that is followed by nothing or a blank line and text.
    const std::string closing = "\n</skill>";
    for (auto at = rest.find(closing); at != std::string::npos; at = rest.find(closing, at + 1)) {
        const std::string after = rest.substr(at + closing.size());
        ParsedSkillBlock block;
        block.name = name;
        block.location = location;
        block.content = rest.substr(0, at);
        if (after.empty()) {
            return block;
        }
        if (after.starts_with("\n\n") && after.size() > 2) {
            const std::string message = trim(after.substr(2));
            if (!message.empty()) {
                block.userMessage = message;
            }
            return block;
        }
    }
    return std::nullopt;
}

std::optional<ParsedSkillBlock> SkillCommandExpander::parse(const std::string& text) const {
    const std::string nameOpen = "<skill name=\"";
    if (!text.starts_with(nameOpen)) {
        return std::nullopt;
    }
    const auto nameEnd = text.find('"', nameOpen.size());
    if (nameEnd == std::string::npos || nameEnd == nameOpen.size()) {
        return std::nullopt;
    }
    const std::string locationOpen = "\" location=\"";
    if (text.compare(nameEnd, locationOpen.size(), locationOpen) != 0) {
        return std::nullopt;
    }
    const std::size_t locationStart = nameEnd + locationOpen.size();
    const auto locationEnd = text.find('"', locationStart);
    if (locationEnd == std::string::npos || locationEnd == locationStart ||
        text.compare(locationEnd, 3, "\">\n") != 0) {
        return std::nullopt;
    }
    return parseTail(text.substr(nameOpen.size(), nameEnd - nameOpen.size()),
                     text.substr(locationStart, locationEnd - locationStart), text.substr(locationEnd + 3));
}
