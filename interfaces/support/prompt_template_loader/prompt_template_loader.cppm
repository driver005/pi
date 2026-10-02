module;

#include <nlohmann/json.hpp>

export module pi.support.prompt_template_loader;

import std;
export import pi.platform.i_file_system;
export import pi.support.frontmatter_parser;
export import pi.support.path_resolver;
export import pi.types.load_prompt_templates_options;
export import pi.types.load_prompt_templates_result;

/**
 * Loads prompt templates: .md files (non-recursive) from <agentDir>/prompts, <cwd>/.pi/prompts and
 * explicit paths. The name is the file name without .md; the description comes from the
 * frontmatter or the first non-empty line. Port of loadPromptTemplates in core/prompt-templates.ts.
 */
export class PromptTemplateLoader {
public:
    explicit PromptTemplateLoader(IFileSystem& files);

    LoadPromptTemplatesResult load(const LoadPromptTemplatesOptions& options);

private:
    void loadFromDir(const std::string& dir, const std::string& globalDir, const std::string& projectDir,
                     LoadPromptTemplatesResult& out);
    void loadFile(const std::string& path, const std::string& globalDir, const std::string& projectDir,
                  LoadPromptTemplatesResult& out);
    SourceInfo sourceInfo(const std::string& path, const std::string& globalDir,
                          const std::string& projectDir) const;
    bool isUnder(const std::string& target, const std::string& root) const;
    bool endsWith(const std::string& text, const std::string& suffix) const;
    std::string trim(const std::string& text) const;

    IFileSystem& m_files;
    FrontmatterParser m_frontmatter;
    PathResolver m_paths;
};

PromptTemplateLoader::PromptTemplateLoader(IFileSystem& files)
    : m_files(files), m_paths(files.homeDirectory()) {}

bool PromptTemplateLoader::endsWith(const std::string& text, const std::string& suffix) const {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string PromptTemplateLoader::trim(const std::string& text) const {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool PromptTemplateLoader::isUnder(const std::string& target, const std::string& root) const {
    return target == root || target.rfind(root.back() == '/' ? root : root + "/", 0) == 0;
}

SourceInfo PromptTemplateLoader::sourceInfo(const std::string& path, const std::string& globalDir,
                                            const std::string& projectDir) const {
    SourceInfo info;
    info.path = path;
    info.source = "local";
    if (isUnder(path, globalDir)) {
        info.scope = "user";
        info.baseDir = globalDir;
    } else if (isUnder(path, projectDir)) {
        info.scope = "project";
        info.baseDir = projectDir;
    } else {
        const auto stat = m_files.stat(path);
        const auto slash = path.find_last_of('/');
        info.baseDir = stat && stat->isDirectory ? path : (slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash));
    }
    return info;
}

void PromptTemplateLoader::loadFile(const std::string& path, const std::string& globalDir,
                                    const std::string& projectDir, LoadPromptTemplatesResult& out) {
    auto raw = m_files.readFile(path);
    if (!raw) {
        out.diagnostics.push_back(ResourceDiagnostic{"warning", raw.error().message, path, std::nullopt});
        return;
    }
    auto document = m_frontmatter.parse(*raw);
    if (!document) {
        out.diagnostics.push_back(ResourceDiagnostic{"warning", document.error().message, path, std::nullopt});
        return;
    }
    PromptTemplate templ;
    const auto slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (endsWith(name, ".md")) {
        name.resize(name.size() - 3);
    }
    templ.name = name;
    const Json& frontmatter = document->frontmatter;
    if (frontmatter.contains("description") && frontmatter["description"].is_string()) {
        templ.description = frontmatter["description"].get<std::string>();
    }
    if (templ.description.empty()) {
        std::size_t start = 0;
        while (start <= document->body.size()) {
            const std::size_t end = document->body.find('\n', start);
            const std::string line = document->body.substr(
                start, end == std::string::npos ? std::string::npos : end - start);
            if (!trim(line).empty()) {
                templ.description = line.substr(0, 60);
                if (line.size() > 60) {
                    templ.description += "...";
                }
                break;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    if (frontmatter.contains("argument-hint") && frontmatter["argument-hint"].is_string() &&
        !frontmatter["argument-hint"].get<std::string>().empty()) {
        templ.argumentHint = frontmatter["argument-hint"].get<std::string>();
    }
    templ.content = document->body;
    templ.sourceInfo = sourceInfo(path, globalDir, projectDir);
    templ.filePath = path;
    out.templates.push_back(std::move(templ));
}

void PromptTemplateLoader::loadFromDir(const std::string& dir, const std::string& globalDir,
                                       const std::string& projectDir, LoadPromptTemplatesResult& out) {
    auto names = m_files.listDirectory(dir);
    if (!names) {
        return;
    }
    std::sort(names->begin(), names->end());
    for (const auto& name : *names) {
        const std::string path = dir + "/" + name;
        const auto info = m_files.stat(path);
        if (info && info->isFile && endsWith(name, ".md")) {
            loadFile(path, globalDir, projectDir, out);
        }
    }
}

LoadPromptTemplatesResult PromptTemplateLoader::load(const LoadPromptTemplatesOptions& options) {
    const std::string cwd = m_paths.resolveToCwd(options.cwd, "/");
    const std::string agentDir = m_paths.resolveToCwd(options.agentDir, "/");
    const std::string globalDir = agentDir + "/prompts";
    const std::string projectDir = cwd + "/.pi/prompts";
    LoadPromptTemplatesResult out;
    if (options.includeDefaults) {
        loadFromDir(globalDir, globalDir, projectDir, out);
        loadFromDir(projectDir, globalDir, projectDir, out);
    }
    for (const auto& raw : options.promptPaths) {
        const std::string path = m_paths.resolveToCwd(trim(raw), cwd);
        const auto info = m_files.stat(path);
        if (!info) {
            continue;
        }
        if (info->isDirectory) {
            loadFromDir(path, globalDir, projectDir, out);
        } else if (info->isFile && endsWith(path, ".md")) {
            loadFile(path, globalDir, projectDir, out);
        }
    }
    return out;
}
