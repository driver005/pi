export module pi.support.pi_prompt_extension;

import std;
export import pi.support.resource_set_cache;
export import pi.support.system_prompt_builder;
export import pi.support.tool_set_cache;
export import pi.types.extension;

/**
 * pi's system prompt as one durable extension: the sections of SystemPromptBuilder for the request's offered tools and the
 * conversation's directory (its agent `cwd`, else the default). Tool snippets and guidelines come from the tools of that
 * directory, context files, skills, SYSTEM.md and APPEND_SYSTEM.md from its resources. Sections without content are
 * omitted. Port of durable/prompt.ts.
 */
export class PiPromptExtension {
public:
    PiPromptExtension(std::shared_ptr<ToolSetCache> tools, std::shared_ptr<ResourceSetCache> resources, std::string defaultCwd)
        : m_tools(std::move(tools)),
          m_resources(std::move(resources)),
          m_defaultCwd(std::move(defaultCwd)) {}

    Extension extension() const {
        Extension extension;
        extension.name = "pi-prompt";
        for (const std::string key : {"preamble", "tools", "rules", "docs", "addendum", "project_context", "skills", "cwd"}) {
            PromptSection section;
            section.key = key;
            // The built sections carry their own tags.
            section.tag = false;
            section.render = [self = *this, key](const PromptInput& input) { return self.render(key, input); };
            extension.sections.push_back(std::move(section));
        }
        return extension;
    }

private:
    Result<std::optional<std::string>> render(const std::string& key, const PromptInput& input) const {
        const std::string cwd = input.agent && input.agent->cwd ? *input.agent->cwd : m_defaultCwd;
        const LoadedResources resources = m_resources->resources(cwd);
        BuildSystemPromptOptions options;
        options.cwd = cwd;
        options.skills = resources.skills;
        options.contextFiles = resources.contextFiles;
        options.customPrompt = resources.systemPrompt;
        for (std::size_t i = 0; i < resources.appendSystemPrompt.size(); ++i) {
            options.appendSystemPrompt += (i == 0 ? "" : "\n\n") + resources.appendSystemPrompt[i];
        }
        options.selectedTools.clear();
        if (input.agent) {
            for (const ToolRegistration& tool : input.agent->tools) {
                options.selectedTools.push_back(tool.name);
                describe(options, cwd, tool.name);
            }
        }
        auto sections = m_builder.buildSections(options);
        if (!sections) {
            return std::unexpected(sections.error());
        }
        for (const auto& [name, text] : *sections) {
            if (name == key) {
                return std::optional<std::string>(text);
            }
        }
        return std::optional<std::string>();
    }

    /** Adds the snippet and guidelines the tool of `cwd` contributes to the prompt. */
    void describe(BuildSystemPromptOptions& options, const std::string& cwd, const std::string& name) const {
        const std::shared_ptr<ITool> tool = m_tools->find(cwd, name);
        if (!tool) {
            return;
        }
        const std::string snippet = normalize(tool->promptSnippet());
        if (!snippet.empty()) {
            options.toolSnippets[name] = snippet;
        }
        std::vector<std::string> guidelines;
        for (const std::string& guideline : tool->promptGuidelines()) {
            const std::string trimmed = normalize(guideline);
            if (!trimmed.empty() && std::ranges::find(guidelines, trimmed) == guidelines.end()) {
                guidelines.push_back(trimmed);
            }
        }
        if (!guidelines.empty()) {
            options.toolGuidelines[name] = std::move(guidelines);
        }
    }

    /** Collapses whitespace runs to single spaces and trims the ends. */
    std::string normalize(const std::string& text) const {
        std::string out;
        bool pendingSpace = false;
        for (const char c : text) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                pendingSpace = !out.empty();
            } else {
                out += (pendingSpace ? " " : "");
                out += c;
                pendingSpace = false;
            }
        }
        return out;
    }

    std::shared_ptr<ToolSetCache> m_tools;
    std::shared_ptr<ResourceSetCache> m_resources;
    std::string m_defaultCwd;
    SystemPromptBuilder m_builder;
};
