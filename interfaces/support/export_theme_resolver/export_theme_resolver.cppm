export module pi.support.export_theme_resolver;

import std;
import pi.support.theme_color_parser;
export import pi.types.json;
export import pi.types.result;
export import pi.types.theme_token_set;

/**
 * Resolves a theme file (`{name, appearance?, vars?, colors, export?}`) into what the HTML export
 * needs, in the shape of an entry of the embedded themes:
 * `{appearance, colors: {token: "#rrggbb"}, export: {pageBg, cardBg, infoBg}}`. Variable references
 * are followed, the optional tokens fall back as in TypeScript (scrollbarTrack to muted,
 * scrollbarThumb to text, thinkingMax to thinkingXhigh, searchMatchBg to selectedBg,
 * searchMatchText to text), `""` (terminal default) becomes the guessed terminal colors of the
 * theme's appearance, and the appearance is detected from the colors when the file does not declare
 * it. The export colors are the explicit ones of the file, when they are valid colors, or derived
 * from `userMessageBg`; they end up in the page's CSS, so a value that is not a color is never
 * copied. Key order follows TypeScript's, so the page's CSS variables come out in the same order.
 * Port of the HTML export helpers of modes/interactive/theme/theme.ts and deriveExportColors of
 * core/export-html/index.ts.
 */
export class ExportThemeResolver {
public:
    Result<Json> resolve(const std::string& label, const Json& theme) const {
        if (!theme.is_object() || !theme.contains("colors") || !theme["colors"].is_object()) {
            return std::unexpected(Error{
                "invalid_theme",
                "Invalid theme \"" + label + "\": expected an object with a \"colors\" map."});
        }
        if (const std::string missing = missingTokens(theme["colors"]); !missing.empty()) {
            return std::unexpected(Error{
                "invalid_theme",
                "Invalid theme \"" + label + "\":\n\nMissing required color tokens:\n" + missing +
                    "\n\nPlease add these colors to your theme's \"colors\" object."});
        }
        const Json vars =
            theme.contains("vars") && theme["vars"].is_object() ? theme["vars"] : Json::object();
        auto tokens = collectTokens(theme["colors"], vars);
        if (!tokens) {
            return std::unexpected(Error{
                "invalid_theme", "Invalid theme \"" + label + "\": " + tokens.error().message});
        }
        const std::string appearance = declaredAppearance(theme).value_or(
            detectAppearance(tokens->foregroundLightness, tokens->backgroundLightness));
        const Json colors = assembleColors(*tokens, appearance == "light");
        return Json{{"appearance", appearance},
                    {"colors", colors},
                    {"export", exportColors(theme, vars, colors)}};
    }

private:
    using Token = std::pair<std::string, Json>;

    /** Resolves every token (foregrounds first, then backgrounds, as TypeScript's theme does). */
    Result<ThemeTokenSet> collectTokens(const Json& colors, const Json& vars) const {
        ThemeTokenSet set;
        const std::vector<Token> tokens = orderedTokens(colors);
        for (const bool background : {false, true}) {
            for (const auto& [token, raw] : tokens) {
                if (isBackground(token) != background) {
                    continue;
                }
                auto resolved = resolveReference(raw, vars);
                if (!resolved) {
                    return std::unexpected(resolved.error());
                }
                if (resolved->is_string() && resolved->get<std::string>().empty()) {
                    (background ? set.defaultBackgrounds : set.defaultForegrounds).push_back(token);
                    continue;
                }
                auto color = m_colors.parse(*resolved);
                if (!color) {
                    return std::unexpected(color.error());
                }
                set.concrete.emplace_back(token, *color);
                if (!color->terminalPalette) {
                    (background ? set.backgroundLightness : set.foregroundLightness)
                        .push_back(color->lightness);
                }
            }
        }
        return set;
    }

    /** The colors as hex strings: own colors first, then the terminal defaults of the appearance.
     */
    Json assembleColors(const ThemeTokenSet& set, bool light) const {
        const RgbColor defaultForeground = light ? RgbColor{0, 0, 0} : RgbColor{229, 229, 231};
        const RgbColor defaultBackground = light ? RgbColor{255, 255, 255} : RgbColor{0, 0, 0};
        Json colors = Json::object();
        for (const auto& [token, color] : set.concrete) {
            colors[token] = m_colors.toHex(color.rgb);
        }
        for (const std::string& token : set.defaultForegrounds) {
            colors[token] = m_colors.toHex(defaultForeground);
        }
        for (const std::string& token : set.defaultBackgrounds) {
            colors[token] = m_colors.toHex(defaultBackground);
        }
        return colors;
    }

    /** The color tokens in TypeScript's order: the file's, then the missing optional ones with
     * their fallback references. */
    std::vector<Token> orderedTokens(const Json& colors) const {
        std::vector<Token> out;
        for (const auto& entry : colors.items()) {
            out.emplace_back(entry.key(), entry.value());
        }
        const std::vector<std::pair<std::string, std::string>> fallbacks{
            {"scrollbarTrack", "muted"},
            {"scrollbarThumb", "text"},
            {"thinkingMax", "thinkingXhigh"},
            {"searchMatchBg", "selectedBg"},
            {"searchMatchText", "text"}};
        for (const auto& [token, source] : fallbacks) {
            if (!colors.contains(token)) {
                out.emplace_back(token, colors[source]);
            }
        }
        return out;
    }

    bool isBackground(const std::string& token) const {
        return token == "selectedBg" || token == "searchMatchBg" || token == "userMessageBg" ||
               token == "customMessageBg" || token == "toolPendingBg" || token == "toolSuccessBg" ||
               token == "toolErrorBg";
    }

    std::string missingTokens(const Json& colors) const {
        std::vector<std::string> required{"accent",
                                          "border",
                                          "borderAccent",
                                          "borderMuted",
                                          "success",
                                          "error",
                                          "warning",
                                          "muted",
                                          "dim",
                                          "text",
                                          "thinkingText",
                                          "selectedBg",
                                          "userMessageBg",
                                          "userMessageText",
                                          "customMessageBg",
                                          "customMessageText",
                                          "customMessageLabel",
                                          "toolPendingBg",
                                          "toolSuccessBg",
                                          "toolErrorBg",
                                          "toolTitle",
                                          "toolOutput",
                                          "mdHeading",
                                          "mdLink",
                                          "mdLinkUrl",
                                          "mdCode",
                                          "mdCodeBlock",
                                          "mdCodeBlockBorder",
                                          "mdQuote",
                                          "mdQuoteBorder",
                                          "mdHr",
                                          "mdListBullet",
                                          "toolDiffAdded",
                                          "toolDiffRemoved",
                                          "toolDiffContext",
                                          "syntaxComment",
                                          "syntaxKeyword",
                                          "syntaxFunction",
                                          "syntaxVariable",
                                          "syntaxString",
                                          "syntaxNumber",
                                          "syntaxType",
                                          "syntaxOperator",
                                          "syntaxPunctuation",
                                          "thinkingOff",
                                          "thinkingMinimal",
                                          "thinkingLow",
                                          "thinkingMedium",
                                          "thinkingHigh",
                                          "thinkingXhigh",
                                          "bashMode"};
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
    Result<Json> resolveReference(const Json& value, const Json& vars) const {
        std::set<std::string> visited;
        Json current = value;
        while (true) {
            if (current.is_number()) {
                return current;
            }
            if (!current.is_string()) {
                return std::unexpected(
                    Error{"invalid_theme", "Invalid color value: " + current.dump()});
            }
            const std::string text = current.get<std::string>();
            if (text.empty() || text.starts_with("#") || m_colors.isOkColorFunction(text)) {
                return current;
            }
            if (visited.contains(text)) {
                return std::unexpected(
                    Error{"invalid_theme", "Circular variable reference detected: " + text});
            }
            if (!vars.contains(text)) {
                return std::unexpected(
                    Error{"invalid_theme", "Variable reference not found: " + text});
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

    /** The background a theme is designed for, from the average lightness of its own colors (dark
     * when it has none). */
    std::string detectAppearance(const std::vector<double>& foregrounds,
                                 const std::vector<double>& backgrounds) const {
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

    double average(const std::vector<double>& values) const {
        return std::accumulate(values.begin(), values.end(), 0.0) /
               static_cast<double>(values.size());
    }

    /**
     * The explicit export colors of the file, else derived from userMessageBg. Hex and `oklch()`
     * values pass through to CSS, `okhsl()` and palette indexes become hex; anything that is not a
     * color is replaced by the derived color.
     */
    Json exportColors(const Json& theme, const Json& vars, const Json& colors) const {
        const Json section = theme.contains("export") && theme["export"].is_object()
                                 ? theme["export"]
                                 : Json::object();
        const Json derived = deriveExportColors(colors);
        Json out = Json::object();
        for (const char* key : {"pageBg", "cardBg", "infoBg"}) {
            if (!section.contains(key)) {
                out[key] = derived[key];
                continue;
            }
            const auto resolved = resolveReference(section[key], vars);
            if (!resolved) {
                return derived;
            }
            out[key] = explicitColor(*resolved).value_or(derived[key].get<std::string>());
        }
        return out;
    }

    /** The CSS color for an explicit export value; nothing for `""` and for values that are not
     * colors. */
    std::optional<std::string> explicitColor(const Json& resolved) const {
        if (resolved.is_string() && resolved.get<std::string>().empty()) {
            return std::nullopt;
        }
        const auto color = m_colors.parse(resolved);
        if (!color) {
            return std::nullopt;
        }
        if (resolved.is_number() || m_colors.isOkhsl(resolved.get<std::string>())) {
            return m_colors.toHex(color->rgb);
        }
        return resolved.get<std::string>();
    }

    /** deriveExportColors over the resolved userMessageBg ("#343541" when the theme has none). */
    Json deriveExportColors(const Json& colors) const {
        const std::string base =
            colors.contains("userMessageBg") && !colors["userMessageBg"].get<std::string>().empty()
                ? colors["userMessageBg"].get<std::string>()
                : "#343541";
        const auto parsed = m_colors.parse(base);
        if (!parsed) {
            return Json{{"pageBg", "rgb(24, 24, 30)"},
                        {"cardBg", "rgb(30, 30, 36)"},
                        {"infoBg", "rgb(60, 55, 40)"}};
        }
        const RgbColor color = parsed->rgb;
        const auto rgb = [](double red, double green, double blue) {
            return std::format("rgb({}, {}, {})", static_cast<int>(red), static_cast<int>(green),
                               static_cast<int>(blue));
        };
        const auto scaled = [&](double factor) {
            const auto adjust = [&](double channel) {
                return std::clamp(std::floor(channel * factor + 0.5), 0.0, 255.0);
            };
            return rgb(adjust(color.r), adjust(color.g), adjust(color.b));
        };
        if (luminance(color) > 0.5) {
            return Json{{"pageBg", scaled(0.96)},
                        {"cardBg", base},
                        {"infoBg", rgb(std::min(255.0, color.r + 10), std::min(255.0, color.g + 5),
                                       std::max(0.0, color.b - 20))}};
        }
        return Json{
            {"pageBg", scaled(0.7)},
            {"cardBg", scaled(0.85)},
            {"infoBg", rgb(std::min(255.0, color.r + 20), std::min(255.0, color.g + 15), color.b)}};
    }

    /** Relative luminance (0-1, higher is lighter). */
    double luminance(const RgbColor& color) const {
        const auto linear = [](double channel) {
            const double scaled = channel / 255;
            return scaled <= 0.03928 ? scaled / 12.92 : std::pow((scaled + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) + 0.0722 * linear(color.b);
    }

    ThemeColorParser m_colors;
};
