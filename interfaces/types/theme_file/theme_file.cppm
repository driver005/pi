export module pi.types.theme_file;

import std;
export import pi.types.json;

/** A `*.json` file of a theme directory: its name, and either the parsed theme object or why it
 * could not be used. */
export struct ThemeFile {
    std::string file;
    std::optional<Json> theme;
    std::string problem;
};
