export module pi.support.plugin_discovery;

import std;
export import pi.platform.i_file_system;

/** Finds plugin libraries (`*.so`, `*.dylib`) directly inside a directory, sorted by name. */
export class PluginDiscovery {
public:
    explicit PluginDiscovery(IFileSystem& files)
        : m_files(files) {}

    std::vector<std::string> discover(const std::string& directoryName) {
        std::vector<std::string> paths;
        std::string directory = directoryName;
        while (directory.size() > 1 && directory.ends_with('/')) {
            directory.pop_back();
        }
        const auto names = m_files.listDirectory(directory);
        if (!names) {
            return paths;
        }
        for (const std::string& name : *names) {
            const std::string path = (directory == "/" ? "" : directory) + "/" + name;
            const auto stat = m_files.stat(path);
            if (isLibrary(name) && stat && !stat->isDirectory) {
                paths.push_back(path);
            }
        }
        std::sort(paths.begin(), paths.end());
        return paths;
    }

private:
    bool isLibrary(const std::string& name) const {
        return name.ends_with(".so") || name.ends_with(".dylib");
    }

    IFileSystem& m_files;
};
