export module pi.support.system_prompt_builder;

import std;
export import pi.support.skill_prompt_formatter;
export import pi.support.transcript_normalizer;
export import pi.types.build_system_prompt_options;
export import pi.types.result;
export import pi.types.system_message;
export import pi.types.system_prompt_state;

/**
 * Builds the structured system prompt: ordered named sections (preamble, tools, rules, docs,
 * addendum, project_context, skills, cwd, custom ones), each wrapped in an XML tag of its name
 * except the preamble, so later transcript updates can replace one section. Port of
 * core/system-prompt.ts. The docs section needs docs locations; without a readme path it is omitted.
 */
export class SystemPromptBuilder {
public:
    using Sections = std::vector<std::pair<std::string, std::string>>;

    /** Error for an invalid custom section name (must match [a-z][a-z0-9_-]*, not "preamble"). */
    Result<Sections> buildSections(const BuildSystemPromptOptions& options) const;

    /** A forced prompt is opaque content; otherwise the structured sections. */
    Result<SystemPromptState> buildState(const BuildSystemPromptOptions& options) const;

    /** The prompt rendered the way the transcript's system message replays it. */
    Result<std::string> buildText(const BuildSystemPromptOptions& options) const;

    SystemMessage toSystemMessage(const SystemPromptState& state, std::int64_t timestamp) const;

    /** Section patch turning `previous` into `current`; nullopt value removes a section. */
    std::vector<std::pair<std::string, std::optional<std::string>>> diff(
        const std::vector<std::pair<std::string, std::string>>& previous, const Sections& current) const;

private:
    bool validSectionName(const std::string& name) const;
    std::string renderProjectContext(const std::vector<ContextFile>& files) const;
    std::string buildRules(const BuildSystemPromptOptions& options) const;
    std::string buildTools(const BuildSystemPromptOptions& options) const;
    std::string buildDocs(const BuildSystemPromptOptions& options) const;
    std::string trim(const std::string& text) const;
    bool selected(const BuildSystemPromptOptions& options, const std::string& tool) const;

    SkillPromptFormatter m_skills;
    TranscriptNormalizer m_normalizer;
};

std::string SystemPromptBuilder::trim(const std::string& text) const {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool SystemPromptBuilder::validSectionName(const std::string& name) const {
    if (name.empty() || !(name[0] >= 'a' && name[0] <= 'z')) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

bool SystemPromptBuilder::selected(const BuildSystemPromptOptions& options, const std::string& tool) const {
    return std::find(options.selectedTools.begin(), options.selectedTools.end(), tool) != options.selectedTools.end();
}

std::string SystemPromptBuilder::renderProjectContext(const std::vector<ContextFile>& files) const {
    std::string out = "Project-specific instructions and guidelines:";
    for (const auto& file : files) {
        out += "\n\n<project_instructions path=\"" + file.path + "\">\n" + file.content +
               "\n</project_instructions>";
    }
    return out;
}

std::string SystemPromptBuilder::buildRules(const BuildSystemPromptOptions& options) const {
    std::vector<std::string> rules;
    std::set<std::string> seen;
    const auto add = [&](const std::string& rule) {
        const std::string normalized = trim(rule);
        if (!normalized.empty() && seen.insert(normalized).second) {
            rules.push_back(normalized);
        }
    };
    const bool bash = selected(options, "bash");
    const bool powershell = selected(options, "powershell");
    if ((bash || powershell) && !selected(options, "grep") && !selected(options, "find") && !selected(options, "ls")) {
        if (bash && powershell) {
            add("Use bash or PowerShell for file operations like listing, searching, and finding files");
        } else if (powershell) {
            add("Use PowerShell for file operations like listing, searching, and finding files");
        } else {
            add("Use bash for file operations like ls, rg, find");
        }
    }
    for (const auto& name : options.selectedTools) {
        const auto found = options.toolGuidelines.find(name);
        if (found != options.toolGuidelines.end()) {
            for (const auto& rule : found->second) {
                add(rule);
            }
        }
    }
    for (const auto& rule : options.promptGuidelines) {
        add(rule);
    }
    add("Be concise in your responses");
    add("Show file paths clearly when working with files");
    std::string out;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        out += (i > 0 ? "\n" : "") + std::string("- ") + rules[i];
    }
    return out;
}

std::string SystemPromptBuilder::buildTools(const BuildSystemPromptOptions& options) const {
    std::string list;
    for (const auto& name : options.selectedTools) {
        const auto snippet = options.toolSnippets.find(name);
        if (snippet != options.toolSnippets.end() && !snippet->second.empty()) {
            list += (list.empty() ? "" : "\n") + std::string("- ") + name + ": " + snippet->second;
        }
    }
    if (list.empty()) {
        list = "(none)";
    }
    return list + "\n\nIn addition to the tools above, you may have access to other custom tools depending on the project.";
}

std::string SystemPromptBuilder::buildDocs(const BuildSystemPromptOptions& options) const {
    return "Pi documentation (read only when the user asks about pi itself, its SDK, extensions, themes, skills, or TUI):\n"
           "- Main documentation: " + options.readmePath + "\n- Additional docs: " + options.docsPath +
           "\n- Examples: " + options.examplesPath + " (extensions, custom tools, SDK)\n"
           "- When reading pi docs or examples, resolve docs/... under Additional docs and examples/... under Examples, not the current working directory\n"
           "- When working on pi topics, read the docs and examples, and follow .md cross-references before implementing\n"
           "- Always read pi .md files completely and follow links to related docs";
}

Result<SystemPromptBuilder::Sections> SystemPromptBuilder::buildSections(
    const BuildSystemPromptOptions& options) const {
    for (const auto& [name, content] : options.sections) {
        if (!validSectionName(name) || name == "preamble") {
            return std::unexpected(Error{"invalid_section", "Invalid system prompt section name: " + name});
        }
    }
    Sections raw;
    if (options.customPrompt && !options.customPrompt->empty()) {
        raw.emplace_back("preamble", *options.customPrompt);
    } else {
        raw.emplace_back("preamble",
                         "You are an expert coding assistant operating inside pi, a coding agent harness. You help "
                         "users by reading files, executing commands, editing code, and writing new files.");
        raw.emplace_back("tools", buildTools(options));
        raw.emplace_back("rules", buildRules(options));
        if (!options.readmePath.empty()) {
            raw.emplace_back("docs", buildDocs(options));
        }
    }
    if (!options.appendSystemPrompt.empty()) {
        raw.emplace_back("addendum", options.appendSystemPrompt);
    }
    if (!options.contextFiles.empty()) {
        raw.emplace_back("project_context", renderProjectContext(options.contextFiles));
    }
    const std::string readTool = selected(options, "read") ? "read" : selected(options, "bash") ? "bash" : "";
    if (!readTool.empty() && !options.skills.empty()) {
        const std::string skills = trim(m_skills.format(options.skills, readTool));
        if (!skills.empty()) {
            raw.emplace_back("skills", skills);
        }
    }
    std::string cwd = options.cwd;
    std::replace(cwd.begin(), cwd.end(), '\\', '/');
    raw.emplace_back("cwd", cwd);
    for (const auto& [name, content] : options.sections) {
        if (!content.empty()) {
            const auto existing = std::find_if(raw.begin(), raw.end(), [&](const auto& entry) { return entry.first == name; });
            if (existing != raw.end()) {
                existing->second = content;
            } else {
                raw.emplace_back(name, content);
            }
        }
    }
    Sections sections;
    for (const auto& [name, content] : raw) {
        sections.emplace_back(name, name == "preamble" ? content : "<" + name + ">\n" + content + "\n</" + name + ">");
    }
    return sections;
}

Result<SystemPromptState> SystemPromptBuilder::buildState(const BuildSystemPromptOptions& options) const {
    SystemPromptState state;
    if (options.forceSystemPrompt) {
        state.content = *options.forceSystemPrompt;
        return state;
    }
    auto sections = buildSections(options);
    if (!sections) {
        return std::unexpected(sections.error());
    }
    state.sections = std::move(*sections);
    return state;
}

SystemMessage SystemPromptBuilder::toSystemMessage(const SystemPromptState& state, std::int64_t timestamp) const {
    SystemMessage message;
    message.content = state.content;
    if (state.sections) {
        std::vector<std::pair<std::string, std::optional<std::string>>> sections;
        for (const auto& [name, text] : *state.sections) {
            sections.emplace_back(name, text);
        }
        message.sections = std::move(sections);
    }
    message.timestamp = timestamp;
    return message;
}

Result<std::string> SystemPromptBuilder::buildText(const BuildSystemPromptOptions& options) const {
    auto state = buildState(options);
    if (!state) {
        return std::unexpected(state.error());
    }
    return m_normalizer.systemMessageText(toSystemMessage(*state, 0));
}

std::vector<std::pair<std::string, std::optional<std::string>>> SystemPromptBuilder::diff(
    const std::vector<std::pair<std::string, std::string>>& previous, const Sections& current) const {
    std::vector<std::pair<std::string, std::optional<std::string>>> patch;
    for (const auto& [name, text] : current) {
        const auto old = std::find_if(previous.begin(), previous.end(), [&](const auto& entry) { return entry.first == name; });
        if (old == previous.end() || old->second != text) {
            patch.emplace_back(name, text);
        }
    }
    for (const auto& [name, text] : previous) {
        const auto now = std::find_if(current.begin(), current.end(), [&](const auto& entry) { return entry.first == name; });
        if (now == current.end()) {
            patch.emplace_back(name, std::nullopt);
        }
    }
    return patch;
}
