export module pi.session.prompt_loadout;

import std;
export import pi.agent.i_agent;
export import pi.platform.i_clock;
export import pi.session.i_resource_loader;
export import pi.session.i_session_manager;
export import pi.support.system_prompt_builder;
export import pi.support.transcript_normalizer;
export import pi.tool.i_tool_registry;

/**
 * Owns which tools are active and what the system prompt says about them. Changing the active
 * tools updates the agent and the prompt options; before each request the prompt options are
 * compared with the sections the transcript already holds and only the difference is added as a
 * system message. Port of the tool and prompt handling of AgentSession.
 */
export class PromptLoadout {
public:
    PromptLoadout(IAgent& agent, ISessionManager& session, IToolRegistry& tools, IResourceLoader& resources,
                  const IClock& clock, std::string cwd);

    /** Tools outside the allowlist (when set) or inside the denylist can never be active. */
    void setToolFilter(std::optional<std::set<std::string>> allowed, std::set<std::string> excluded);

    /** Rebuilds the base prompt options from the registry and the loaded resources. */
    void rebuild();

    /** Activates the named tools that are registered and allowed; updates agent and prompt options. */
    void setActiveTools(const std::vector<std::string>& names);
    std::vector<std::string> activeToolNames() const;

    BuildSystemPromptOptions baseOptions() const;

    /**
     * Applies options.selectedTools to the agent and returns the system message that turns the
     * sections in `messages` into the sections of `options`; nullopt when nothing changed.
     */
    std::optional<SystemMessage> prepare(BuildSystemPromptOptions& options,
                                         const std::vector<AgentMessage>& messages);

    /** Prompt text for the current base options. */
    std::string systemPromptText() const;

    /** Re-activates the tool set the transcript declares (after resume or navigation). */
    void restoreFromTranscript();

private:
    bool allowed(const std::string& name) const;
    std::vector<Message> systemMessages(const std::vector<AgentMessage>& messages) const;
    std::string normalizeSnippet(const std::string& text) const;

    IAgent& m_agent;
    ISessionManager& m_session;
    IToolRegistry& m_tools;
    IResourceLoader& m_resources;
    const IClock& m_clock;
    std::string m_cwd;
    SystemPromptBuilder m_builder;
    TranscriptNormalizer m_transcript;

    mutable std::mutex m_mutex;
    std::optional<std::set<std::string>> m_allowed;
    std::set<std::string> m_excluded;
    BuildSystemPromptOptions m_base;
};

PromptLoadout::PromptLoadout(IAgent& agent, ISessionManager& session, IToolRegistry& tools,
                             IResourceLoader& resources, const IClock& clock, std::string cwd)
    : m_agent(agent),
      m_session(session),
      m_tools(tools),
      m_resources(resources),
      m_clock(clock),
      m_cwd(std::move(cwd)) {}

void PromptLoadout::setToolFilter(std::optional<std::set<std::string>> allowedNames, std::set<std::string> excluded) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_allowed = std::move(allowedNames);
    m_excluded = std::move(excluded);
}

bool PromptLoadout::allowed(const std::string& name) const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return (!m_allowed || m_allowed->contains(name)) && !m_excluded.contains(name);
}

/** Snippets are single lines: whitespace runs collapse and the ends are trimmed. */
std::string PromptLoadout::normalizeSnippet(const std::string& text) const {
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

std::vector<std::string> PromptLoadout::activeToolNames() const {
    std::vector<std::string> names;
    for (const auto& tool : m_tools.active()) {
        names.push_back(tool->definition().name);
    }
    return names;
}

void PromptLoadout::rebuild() {
    const LoadedResources resources = m_resources.resources();
    BuildSystemPromptOptions options;
    options.cwd = m_cwd;
    options.skills = resources.skills;
    options.contextFiles = resources.contextFiles;
    options.customPrompt = resources.systemPrompt;
    for (std::size_t i = 0; i < resources.appendSystemPrompt.size(); ++i) {
        options.appendSystemPrompt += (i == 0 ? "" : "\n\n") + resources.appendSystemPrompt[i];
    }
    options.selectedTools = activeToolNames();
    for (const auto& tool : m_tools.all()) {
        const std::string& name = tool->definition().name;
        if (const std::string snippet = normalizeSnippet(tool->promptSnippet()); !snippet.empty()) {
            options.toolSnippets[name] = snippet;
        }
        std::vector<std::string> guidelines;
        for (const auto& guideline : tool->promptGuidelines()) {
            const std::string trimmed = normalizeSnippet(guideline);
            if (!trimmed.empty() && std::ranges::find(guidelines, trimmed) == guidelines.end()) {
                guidelines.push_back(trimmed);
            }
        }
        if (!guidelines.empty()) {
            options.toolGuidelines[name] = std::move(guidelines);
        }
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_base = std::move(options);
}

BuildSystemPromptOptions PromptLoadout::baseOptions() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_base;
}

void PromptLoadout::setActiveTools(const std::vector<std::string>& names) {
    std::vector<std::string> usable;
    for (const auto& name : names) {
        if (m_tools.find(name) != nullptr && allowed(name) && std::ranges::find(usable, name) == usable.end()) {
            usable.push_back(name);
        }
    }
    m_tools.setActive(usable);
    m_agent.setTools(m_tools.active());
    rebuild();
}

std::vector<Message> PromptLoadout::systemMessages(const std::vector<AgentMessage>& messages) const {
    std::vector<Message> out;
    for (const auto& message : messages) {
        if (const auto* system = std::get_if<SystemMessage>(&message)) {
            out.emplace_back(*system);
        }
    }
    return out;
}

std::optional<SystemMessage> PromptLoadout::prepare(BuildSystemPromptOptions& options,
                                                    const std::vector<AgentMessage>& messages) {
    std::vector<std::string> names;
    for (const auto& name : options.selectedTools) {
        if (m_tools.find(name) != nullptr && allowed(name)) {
            names.push_back(name);
        }
    }
    m_tools.setActive(names);
    m_agent.setTools(m_tools.active());
    options.selectedTools = activeToolNames();

    SystemPromptBuilder::Sections previous;
    if (const auto current = m_transcript.currentSystemMessage(systemMessages(messages)); current && current->sections) {
        for (const auto& [name, text] : *current->sections) {
            if (text) {
                previous.emplace_back(name, *text);
            }
        }
    }
    const auto sections = m_builder.buildSections(options);
    if (!sections) {
        return std::nullopt;
    }
    auto diff = m_builder.diff(previous, *sections);
    if (diff.empty()) {
        return std::nullopt;
    }
    SystemMessage message;
    message.content = std::string();
    message.sections = std::move(diff);
    message.timestamp = m_clock.nowMs();
    return message;
}

std::string PromptLoadout::systemPromptText() const {
    const auto text = m_builder.buildText(baseOptions());
    return text ? *text : std::string();
}

void PromptLoadout::restoreFromTranscript() {
    const auto current = m_transcript.currentSystemMessage(systemMessages(m_session.buildSessionContext().messages));
    if (!current) {
        return;
    }
    std::vector<std::string> names;
    if (current->toolsAdded) {
        for (const auto& tool : *current->toolsAdded) {
            names.push_back(tool.name);
        }
    }
    setActiveTools(names);
}
