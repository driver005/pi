export module pi.support.context_file_loader;

import std;
export import pi.platform.i_file_system;
export import pi.support.path_resolver;
export import pi.types.context_file;

/**
 * Collects instruction files for the system prompt: the agent directory's first, then every
 * ancestor of the cwd from the outermost inwards. In each directory the first existing file of
 * AGENTS.override.md, AGENTS.md, AGENTS.MD, CLAUDE.md, CLAUDE.MD is used. Port of
 * loadProjectContextFiles in core/resource-loader.ts (worktree shadowing is not ported).
 */
export class ContextFileLoader {
public:
    explicit ContextFileLoader(IFileSystem& files)
        : m_files(files),
          m_paths(files.homeDirectory()) {}

    std::vector<ContextFile> load(const std::string& cwd, const std::string& agentDir) {
        std::vector<ContextFile> files;
        std::set<std::string> seen;
        if (auto global = loadFromDir(m_paths.resolveToCwd(agentDir, "/"))) {
            seen.insert(global->path);
            files.push_back(std::move(*global));
        }
        std::vector<ContextFile> ancestors;
        std::string current = m_paths.resolveToCwd(cwd, "/");
        while (true) {
            if (auto file = loadFromDir(current); file && !seen.contains(file->path)) {
                seen.insert(file->path);
                ancestors.insert(ancestors.begin(), std::move(*file));
            }
            const auto slash = current.find_last_of('/');
            const std::string parent = slash == std::string::npos || slash == 0 ? "/" : current.substr(0, slash);
            if (parent == current) {
                break;
            }
            current = parent;
        }
        for (auto& file : ancestors) {
            files.push_back(std::move(file));
        }
        return files;
    }

    /** The first candidate file of one directory. */
    std::optional<ContextFile> loadFromDir(const std::string& dir) {
        for (const char* name : {"AGENTS.override.md", "AGENTS.md", "AGENTS.MD", "CLAUDE.md", "CLAUDE.MD"}) {
            const std::string path = (dir == "/" ? "" : dir) + "/" + name;
            const auto info = m_files.stat(path);
            if (!info || !info->isFile) {
                continue;
            }
            auto content = m_files.readFile(path);
            if (!content) {
                continue;
            }
            std::string text = *content;
            if (text.rfind("\xEF\xBB\xBF", 0) == 0) {
                text.erase(0, 3);
            }
            return ContextFile{path, std::move(text)};
        }
        return std::nullopt;
    }

private:
    IFileSystem& m_files;
    PathResolver m_paths;
};
