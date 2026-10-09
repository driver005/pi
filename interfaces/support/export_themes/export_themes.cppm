export module pi.support.export_themes;

import std;
export import pi.platform.i_file_system;
import pi.support.export_theme_resolver;
import pi.support.text_trimmer;
export import pi.types.json;
export import pi.types.result;
export import pi.types.theme_file;

/**
 * The themes an HTML export can use, by name: the built-in ones (`dark`, `light`, already resolved,
 * from the embedded assets) first, then the theme files of the given directories in order
 * (`<dir>/<name>.json`, else the first `*.json` whose `name` is the name), which
 * ExportThemeResolver resolves. Names with `/` are refused: it is reserved for the light/dark theme
 * setting. A theme file that is not a JSON object is reported when it is the one asked for, and
 * skipped when listing. Each directory is read once. Each entry is `{appearance, colors, export}`.
 * Port of the theme lookup of modes/interactive/theme/theme.ts.
 */
export class ExportThemes {
public:
    ExportThemes(std::string builtinJson, IFileSystem& files, std::vector<std::string> directories)
        : m_builtin(Json::parse(builtinJson, nullptr, false)),
          m_files(files),
          m_directories(std::move(directories)) {}

    Result<Json> find(const std::string& name) {
        if (name.contains('/')) {
            return std::unexpected(
                Error{"invalid_theme",
                      "Invalid theme name \"" + name +
                          "\": theme names cannot contain \"/\" because it is reserved for "
                          "automatic light/dark theme settings."});
        }
        if (m_builtin.is_object() && m_builtin.contains(name)) {
            return m_builtin[name];
        }
        for (const std::string& directory : m_directories) {
            const std::vector<ThemeFile>& files = scan(directory);
            const auto direct = std::ranges::find(files, name + ".json", &ThemeFile::file);
            if (direct != files.end()) {
                return resolve(directory, name, *direct);
            }
            const auto named = std::ranges::find_if(files, [&](const ThemeFile& file) {
                return file.theme && file.theme->value("name", Json()) == Json(name);
            });
            if (named != files.end()) {
                return resolve(directory, name, *named);
            }
        }
        return std::unexpected(Error{
            "unknown_theme", "Unknown theme \"" + name + "\" (available: " + available() + ")"});
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
            for (const ThemeFile& file : scan(directory)) {
                if (!file.theme) {
                    continue;
                }
                const Json declared = file.theme->value("name", Json());
                const std::string name = declared.is_string()
                                             ? declared.get<std::string>()
                                             : file.file.substr(0, file.file.size() - 5);
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
    Result<Json> resolve(const std::string& directory, const std::string& name,
                         const ThemeFile& file) {
        if (!file.theme) {
            return std::unexpected(Error{
                "invalid_theme",
                "Failed to parse theme " + directory + "/" + file.file + ": " + file.problem});
        }
        return m_resolver.resolve(name, *file.theme);
    }

    /** The `*.json` files of a directory in name order, read and parsed once. */
    const std::vector<ThemeFile>& scan(const std::string& directory) {
        const auto cached = m_scans.find(directory);
        if (cached != m_scans.end()) {
            return cached->second;
        }
        std::vector<ThemeFile> files;
        auto names = m_files.listDirectory(directory);
        if (names) {
            std::ranges::sort(*names);
            for (const std::string& file : *names) {
                if (file.size() > 5 && file.ends_with(".json")) {
                    files.push_back(read(directory + "/" + file, file));
                }
            }
        }
        return m_scans.emplace(directory, std::move(files)).first->second;
    }

    ThemeFile read(const std::string& path, const std::string& file) {
        ThemeFile out;
        out.file = file;
        auto text = m_files.readFile(path);
        if (!text) {
            out.problem = text.error().message;
            return out;
        }
        Json parsed = Json::parse(m_text.stripBom(*text), nullptr, false);
        if (parsed.is_discarded()) {
            out.problem = "not valid JSON";
        } else if (!parsed.is_object()) {
            out.problem = "not a JSON object";
        } else {
            out.theme = std::move(parsed);
        }
        return out;
    }

    Json m_builtin;
    IFileSystem& m_files;
    std::vector<std::string> m_directories;
    ExportThemeResolver m_resolver;
    TextTrimmer m_text;
    std::map<std::string, std::vector<ThemeFile>> m_scans;
};
