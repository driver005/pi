export module pi.session.resource_loader;

import std;
export import pi.platform.i_file_system;
export import pi.session.i_resource_loader;
export import pi.session.i_settings_manager;
export import pi.support.context_file_loader;
export import pi.support.path_resolver;
export import pi.support.prompt_template_loader;
export import pi.support.skill_loader;
export import pi.types.resource_loader_options;

/**
 * IResourceLoader over the file system: context files, skills, prompt templates and the system
 * prompt files (SYSTEM.md and APPEND_SYSTEM.md in the project's .pi directory or the agent
 * directory). Project-level resources load only for a trusted project. Port of the headless part
 * of DefaultResourceLoader; extensions and themes are not resources here.
 */
export class ResourceLoader : public IResourceLoader {
public:
    ResourceLoader(ResourceLoaderOptions options, ISettingsManager& settings, IFileSystem& files);

    LoadedResources resources() const override;
    Result<void> reload() override;

private:
    void loadSkills(bool trusted, LoadedResources& out);
    void loadTemplates(bool trusted, LoadedResources& out);
    std::optional<std::string> discover(const std::string& name, bool trusted);
    std::optional<std::string> promptInput(const std::optional<std::string>& source);
    std::string stripBom(const std::string& text) const;

    ResourceLoaderOptions m_options;
    ISettingsManager& m_settings;
    IFileSystem& m_files;
    SkillLoader m_skills;
    PromptTemplateLoader m_templates;
    ContextFileLoader m_contextFiles;

    mutable std::mutex m_mutex;
    LoadedResources m_loaded;
};

ResourceLoader::ResourceLoader(ResourceLoaderOptions options, ISettingsManager& settings, IFileSystem& files)
    : m_options(std::move(options)),
      m_settings(settings),
      m_files(files),
      m_skills(files),
      m_templates(files),
      m_contextFiles(files) {}

LoadedResources ResourceLoader::resources() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_loaded;
}

void ResourceLoader::loadSkills(bool trusted, LoadedResources& out) {
    LoadSkillsOptions options;
    options.cwd = m_options.cwd;
    options.agentDir = m_options.agentDir;
    options.includeDefaults = !m_options.noSkills;
    options.includeProject = trusted;
    options.skillPaths = m_options.noSkills ? std::vector<std::string>{} : m_settings.view().skillPaths();
    options.skillPaths.insert(options.skillPaths.end(), m_options.additionalSkillPaths.begin(),
                              m_options.additionalSkillPaths.end());
    LoadSkillsResult result = m_skills.load(options);
    out.skills = std::move(result.skills);
    out.diagnostics.insert(out.diagnostics.end(), result.diagnostics.begin(), result.diagnostics.end());
}

void ResourceLoader::loadTemplates(bool trusted, LoadedResources& out) {
    LoadPromptTemplatesOptions options;
    options.cwd = m_options.cwd;
    options.agentDir = m_options.agentDir;
    options.includeDefaults = !m_options.noPromptTemplates;
    options.includeProject = trusted;
    options.promptPaths = m_options.noPromptTemplates ? std::vector<std::string>{}
                                                      : m_settings.view().promptTemplatePaths();
    options.promptPaths.insert(options.promptPaths.end(), m_options.additionalPromptTemplatePaths.begin(),
                               m_options.additionalPromptTemplatePaths.end());
    LoadPromptTemplatesResult result = m_templates.load(options);
    out.promptTemplates = std::move(result.templates);
    out.diagnostics.insert(out.diagnostics.end(), result.diagnostics.begin(), result.diagnostics.end());
}

std::string ResourceLoader::stripBom(const std::string& text) const {
    return text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text;
}

/** A trusted project's file wins over the agent directory's. */
std::optional<std::string> ResourceLoader::discover(const std::string& name, bool trusted) {
    const std::string project = m_options.cwd + "/.pi/" + name;
    if (trusted && m_files.exists(project)) {
        return project;
    }
    const std::string global = m_options.agentDir + "/" + name;
    return m_files.exists(global) ? std::optional<std::string>(global) : std::nullopt;
}

/** A source that is an existing file means "use its content"; anything else is literal text. */
std::optional<std::string> ResourceLoader::promptInput(const std::optional<std::string>& source) {
    if (!source || source->empty()) {
        return std::nullopt;
    }
    if (m_files.exists(*source)) {
        if (const auto content = m_files.readFile(*source)) {
            return stripBom(*content);
        }
    }
    return source;
}

Result<void> ResourceLoader::reload() {
    const bool trusted = m_settings.projectTrusted();
    LoadedResources next;
    loadSkills(trusted, next);
    loadTemplates(trusted, next);
    if (!m_options.noContextFiles) {
        next.contextFiles = m_contextFiles.load(m_options.cwd, m_options.agentDir);
    }
    next.systemPrompt = promptInput(m_options.systemPrompt ? m_options.systemPrompt : discover("SYSTEM.md", trusted));
    std::vector<std::string> appendSources;
    if (m_options.appendSystemPrompt) {
        appendSources = *m_options.appendSystemPrompt;
    } else if (const auto found = discover("APPEND_SYSTEM.md", trusted)) {
        appendSources.push_back(*found);
    }
    for (const auto& source : appendSources) {
        if (const auto text = promptInput(source)) {
            next.appendSystemPrompt.push_back(*text);
        }
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_loaded = std::move(next);
    return {};
}
