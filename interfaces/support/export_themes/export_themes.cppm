export module pi.support.export_themes;

import std;
export import pi.platform.i_file_system;
import pi.support.export_theme_resolver;
export import pi.types.json;
export import pi.types.result;

/**
 * The themes an HTML export can use, by name: the built-in ones (`dark`, `light`, already resolved, from the embedded assets)
 * first, then the theme files of the given directories in order (`<dir>/<name>.json`, else the first `*.json` whose `name` is the
 * name), which ExportThemeResolver resolves. Names with `/` are refused: it is reserved for the light/dark theme setting. Each
 * entry is `{appearance, colors, export}`. Port of the theme lookup of modes/interactive/theme/theme.ts.
 */
export class ExportThemes {
public:
    ExportThemes(std::string builtinJson, IFileSystem& files, std::vector<std::string> directories)
        : m_builtin(Json::parse(builtinJson, nullptr, false)),
          m_files(files),
          m_directories(std::move(directories)) {}

    Result<Json> find(const std::string& name) {
        if (name.contains('/')) {
            return std::unexpected(Error{"invalid_theme", "Invalid theme name \"" + name + "\": theme names cannot contain \"/\" because it is reserved for automatic light/dark theme settings."});
        }
        if (m_builtin.is_object() && m_builtin.contains(name)) {
            return m_builtin[name];
        }
        for (const std::string& directory : m_directories) {
            if (const auto file = locate(directory, name)) {
                return m_resolver.resolve(name, *file);
            }
        }
        return std::unexpected(Error{"unknown_theme", "Unknown theme \"" + name + "\" (available: " + available() + ")"});
    }

    /** The names of the built-in themes, then of the custom themes found in the directories. */
    std::string available() {
        std::vector<std::string> names;
        if (m_builtin.is_object()) {
            for (const auto& entry : m_builtin.items()) {
                names.push_back(entry.key());
            }
        }
        for (const std::string& directory : m_directories) {
            for (const std::string& file : jsonFiles(directory)) {
                const auto theme = read(directory + "/" + file);
                const std::string name = theme && theme->value("name", Json()).is_string() ? theme->at("name").get<std::string>() : file.substr(0, file.size() - 5);
                if (std::ranges::find(names, name) == names.end()) {
                    names.push_back(name);
                }
            }
        }
        std::string out;
        for (const std::string& name : names) {
            out += (out.empty() ? "" : ", ") + name;
        }
        return out;
    }

private:
    std::optional<Json> locate(const std::string& directory, const std::string& name) {
        if (auto direct = read(directory + "/" + name + ".json")) {
            return direct;
        }
        for (const std::string& file : jsonFiles(directory)) {
            if (auto theme = read(directory + "/" + file); theme && theme->value("name", Json()) == Json(name)) {
                return theme;
            }
        }
        return std::nullopt;
    }

    std::vector<std::string> jsonFiles(const std::string& directory) {
        std::vector<std::string> out;
        const auto names = m_files.listDirectory(directory);
        if (!names) {
            return out;
        }
        for (const std::string& file : *names) {
            if (file.size() > 5 && file.ends_with(".json")) {
                out.push_back(file);
            }
        }
        std::ranges::sort(out);
        return out;
    }

    /** The parsed theme file; nothing when it is missing or not JSON. */
    std::optional<Json> read(const std::string& path) {
        auto text = m_files.readFile(path);
        if (!text) {
            return std::nullopt;
        }
        std::string content = std::move(*text);
        if (content.starts_with("\xEF\xBB\xBF")) {
            content.erase(0, 3);
        }
        Json parsed = Json::parse(content, nullptr, false);
        if (parsed.is_discarded()) {
            return std::nullopt;
        }
        return parsed;
    }

    Json m_builtin;
    IFileSystem& m_files;
    std::vector<std::string> m_directories;
    ExportThemeResolver m_resolver;
};
