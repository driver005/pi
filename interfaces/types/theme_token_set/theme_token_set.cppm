export module pi.types.theme_token_set;

import std;
export import pi.types.theme_color;

/**
 * The color tokens of a theme file after reference resolution: the ones with a color of their own
 * in the order TypeScript lists them, the ones set to `""` (terminal default), and the lightness of
 * the foreground and background colors for detecting the appearance.
 */
export struct ThemeTokenSet {
    std::vector<std::pair<std::string, ThemeColor>> concrete;
    std::vector<std::string> defaultForegrounds;
    std::vector<std::string> defaultBackgrounds;
    std::vector<double> foregroundLightness;
    std::vector<double> backgroundLightness;
};
