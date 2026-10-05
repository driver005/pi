export module pi.support.export_theme_resolver;

import std;
import pi.support.theme_color_parser;
export import pi.types.json;
export import pi.types.result;

/**
 * Resolves a theme file (`{name, appearance?, vars?, colors, export?}`) into what the HTML export needs, in the shape of an entry
 * of the embedded themes: `{appearance, colors: {token: "#rrggbb"}, export: {pageBg, cardBg, infoBg}}`. Variable references are
 * followed, the optional tokens fall back as in TypeScript (scrollbarTrack to muted, scrollbarThumb to text, thinkingMax to
 * thinkingXhigh, searchMatchBg to selectedBg, searchMatchText to text), `""` (terminal default) becomes the guessed terminal
 * colors of the theme's appearance, and the appearance is detected from the colors when the file does not declare it. The export
 * colors are the explicit ones of the file or derived from `userMessageBg`. Key order follows TypeScript's, so the page's CSS
 * variables come out in the same order. Port of the HTML export helpers of modes/interactive/theme/theme.ts and
 * deriveExportColors of core/export-html/index.ts.
 */
export class ExportThemeResolver {
public:
    Result<Json> resolve(const std::string& label, const Json& theme) const {
        if (!theme.is_object() || !theme.contains("colors") || !theme["colors"].is_object()) {
            return std::unexpected(Error{"invalid_theme", "Invalid theme \"" + label + "\": expected an object with a \"colors\" map."});
        }
        if (const std::string missing = missingTokens(theme["colors"]); !missing.empty()) {
            return std::unexpected(Error{"invalid_theme", "Invalid theme \"" + label + "\":\n\nMissing required color tokens:\n" + missing +
                                                              "\n\nPlease add these colors to your theme's \"colors\" object."});
        }
        const Json vars = theme.contains("vars") && theme["vars"].is_object() ? theme["vars"] : Json::object();
        std::vector<std::pair<std::string, ThemeColor>> concrete;
        std::vector<std::string> defaultForegrounds;
        std::vector<std::string> defaultBackgrounds;
        std::vector<double> foregroundLightness;
        std::vector<double> backgroundLightness;
        for (const bool background : {false, true}) {
            for (const auto& [token, raw] : tokens(theme["colors"])) {
                if (isBackground(token) != background) {
                    continue;
                }
                auto resolved = resolveRef(raw, vars);
                if (!resolved) {
                    return std::unexpected(Error{"invalid_theme", "Invalid theme \"" + label + "\": " + resolved.error().message});
                }
                if (resolved->is_string() && resolved->get<std::string>().empty()) {
                    (background ? defaultBackgrounds : defaultForegrounds).push_back(token);
                    continue;
                }
                auto color = m_colors.parse(*resolved);
                if (!color) {
                    return std::unexpected(Error{"invalid_theme", "Invalid theme \"" + label + "\": " + color.error().message});
                }
                concrete.emplace_back(token, *color);
                if (!color->terminalPalette) {
                    (background ? backgroundLightness : foregroundLightness).push_back(color->lightness);
                }
            }
        }
        const std::string appearance = declaredAppearance(theme).value_or(detect(foregroundLightness, backgroundLightness));
        const bool light = appearance == "light";
        const RgbColor foreground = light ? RgbColor{0, 0, 0} : RgbColor{229, 229, 231};
        const RgbColor background = light ? RgbColor{255, 255, 255} : RgbColor{0, 0, 0};
        Json colors = Json::object();
        for (const auto& [token, color] : concrete) {
            colors[token] = m_colors.toHex(color.rgb);
        }
        for (const std::string& token : defaultForegrounds) {
            colors[token] = m_colors.toHex(foreground);
        }
        for (const std::string& token : defaultBackgrounds) {
            colors[token] = m_colors.toHex(background);
        }
        return Json{{"appearance", appearance}, {"colors", colors}, {"export", exportColors(theme, vars, colors)}};
    }

private:
    using Token = std::pair<std::string, Json>;

    /** The color tokens in TypeScript's order: the file's, then the missing optional ones with their fallback references. */
    std::vector<Token> tokens(const Json& colors) const {
        std::vector<Token> out;
        for (const auto& entry : colors.items()) {
            out.emplace_back(entry.key(), entry.value());
        }
        const std::vector<std::pair<std::string, std::string>> fallbacks{{"scrollbarTrack", "muted"}, {"scrollbarThumb", "text"}, {"thinkingMax", "thinkingXhigh"},
                                                                        {"searchMatchBg", "selectedBg"}, {"searchMatchText", "text"}};
        for (const auto& [token, source] : fallbacks) {
            if (!colors.contains(token)) {
                out.emplace_back(token, colors[source]);
            }
        }
        return out;
    }

    bool isBackground(const std::string& token) const {
        return token == "selectedBg" || token == "searchMatchBg" || token == "userMessageBg" || token == "customMessageBg" || token == "toolPendingBg" ||
               token == "toolSuccessBg" || token == "toolErrorBg";
    }

    std::string missingTokens(const Json& colors) const {
        std::vector<std::string> required{"accent", "border", "borderAccent", "borderMuted", "success", "error", "warning", "muted", "dim", "text", "thinkingText",
                                          "selectedBg", "userMessageBg", "userMessageText", "customMessageBg", "customMessageText", "customMessageLabel",
                                          "toolPendingBg", "toolSuccessBg", "toolErrorBg", "toolTitle", "toolOutput", "mdHeading", "mdLink", "mdLinkUrl",
                                          "mdCode", "mdCodeBlock", "mdCodeBlockBorder", "mdQuote", "mdQuoteBorder", "mdHr", "mdListBullet", "toolDiffAdded",
                                          "toolDiffRemoved", "toolDiffContext", "syntaxComment", "syntaxKeyword", "syntaxFunction", "syntaxVariable",
                                          "syntaxString", "syntaxNumber", "syntaxType", "syntaxOperator", "syntaxPunctuation", "thinkingOff", "thinkingMinimal",
                                          "thinkingLow", "thinkingMedium", "thinkingHigh", "thinkingXhigh", "bashMode"};
        std::ranges::sort(required);
        std::string out;
        for (const std::string& token : required) {
            if (!colors.contains(token)) {
                out += (out.empty() ? "" : "\n") + std::string("  - ") + token;
            }
        }
        return out;
    }

    /** Follows variable references to a color value (resolveVarRefs). */
    Result<Json> resolveRef(const Json& value, const Json& vars) const {
        std::set<std::string> visited;
        Json current = value;
        while (true) {
            if (current.is_number()) {
                return current;
            }
            if (!current.is_string()) {
                return std::unexpected(Error{"invalid_theme", "Invalid color value: " + current.dump()});
            }
            const std::string text = current.get<std::string>();
            if (text.empty() || text.starts_with("#") || std::regex_search(text, std::regex("^ok(lch|hsl)\\(", std::regex::icase))) {
                return current;
            }
            if (visited.contains(text)) {
                return std::unexpected(Error{"invalid_theme", "Circular variable reference detected: " + text});
            }
            if (!vars.contains(text)) {
                return std::unexpected(Error{"invalid_theme", "Variable reference not found: " + text});
            }
            visited.insert(text);
            current = vars[text];
        }
    }

    std::optional<std::string> declaredAppearance(const Json& theme) const {
        if (theme.contains("appearance") && theme["appearance"].is_string()) {
            const std::string value = theme["appearance"].get<std::string>();
            if (value == "dark" || value == "light") {
                return value;
            }
        }
        return std::nullopt;
    }

    /** The background a theme is designed for, from the average lightness of its own colors (dark when it has none). */
    std::string detect(const std::vector<double>& foregrounds, const std::vector<double>& backgrounds) const {
        const auto average = [](const std::vector<double>& values) { return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size()); };
        if (!foregrounds.empty() && !backgrounds.empty()) {
            return average(backgrounds) < average(foregrounds) ? "dark" : "light";
        }
        if (!backgrounds.empty()) {
            return average(backgrounds) < 0.5 ? "dark" : "light";
        }
        if (!foregrounds.empty()) {
            return average(foregrounds) > 0.5 ? "dark" : "light";
        }
        return "dark";
    }

    /** The explicit export colors of the file (hex and oklch() pass through to CSS, okhsl() and palette indexes become hex), else derived from userMessageBg. */
    Json exportColors(const Json& theme, const Json& vars, const Json& colors) const {
        const Json section = theme.contains("export") && theme["export"].is_object() ? theme["export"] : Json::object();
        std::map<std::string, std::string> explicitColors;
        for (const char* key : {"pageBg", "cardBg", "infoBg"}) {
            if (!section.contains(key)) {
                continue;
            }
            auto resolved = resolveRef(section[key], vars);
            if (!resolved) {
                return derive(colors);
            }
            if (resolved->is_number()) {
                auto color = m_colors.parse(*resolved);
                if (!color) {
                    return derive(colors);
                }
                explicitColors[key] = m_colors.toHex(color->rgb);
            } else if (!resolved->get<std::string>().empty()) {
                const std::string text = resolved->get<std::string>();
                if (std::regex_search(text, std::regex("^okhsl\\(", std::regex::icase))) {
                    auto color = m_colors.parse(*resolved);
                    if (!color) {
                        return derive(colors);
                    }
                    explicitColors[key] = m_colors.toHex(color->rgb);
                } else {
                    explicitColors[key] = text;
                }
            }
        }
        Json derived = derive(colors);
        Json out = Json::object();
        for (const char* key : {"pageBg", "cardBg", "infoBg"}) {
            out[key] = explicitColors.contains(key) ? Json(explicitColors[key]) : derived[key];
        }
        return out;
    }

    /** deriveExportColors over the resolved userMessageBg ("#343541" when the theme has none). */
    Json derive(const Json& colors) const {
        const std::string base = colors.contains("userMessageBg") && !colors["userMessageBg"].get<std::string>().empty() ? colors["userMessageBg"].get<std::string>() : "#343541";
        const auto parsed = m_colors.parse(base);
        if (!parsed) {
            return Json{{"pageBg", "rgb(24, 24, 30)"}, {"cardBg", "rgb(30, 30, 36)"}, {"infoBg", "rgb(60, 55, 40)"}};
        }
        const RgbColor c = parsed->rgb;
        const auto rgb = [](double r, double g, double b) { return std::format("rgb({}, {}, {})", static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)); };
        const auto scaled = [&](double factor) {
            const auto adjust = [&](double channel) { return std::min(255.0, std::max(0.0, std::floor(channel * factor + 0.5))); };
            return rgb(adjust(c.r), adjust(c.g), adjust(c.b));
        };
        if (luminance(c) > 0.5) {
            return Json{{"pageBg", scaled(0.96)}, {"cardBg", base}, {"infoBg", rgb(std::min(255.0, c.r + 10), std::min(255.0, c.g + 5), std::max(0.0, c.b - 20))}};
        }
        return Json{{"pageBg", scaled(0.7)}, {"cardBg", scaled(0.85)}, {"infoBg", rgb(std::min(255.0, c.r + 20), std::min(255.0, c.g + 15), c.b)}};
    }

    /** Relative luminance (0-1, higher is lighter). */
    double luminance(const RgbColor& color) const {
        const auto linear = [](double channel) {
            const double s = channel / 255;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) + 0.0722 * linear(color.b);
    }

    ThemeColorParser m_colors;
};
