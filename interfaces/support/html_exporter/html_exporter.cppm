export module pi.support.html_exporter;

import std;
export import pi.platform.i_base64_codec;
export import pi.types.export_assets;
export import pi.types.json;
export import pi.types.result;

/**
 * Renders a session as one self-contained HTML page: the template with the theme's CSS variables, the page and card colors, the
 * session data (header, entries, leaf, system prompt, tools) as base64 JSON, and the inlined libraries; the page's own script draws
 * the conversation and its tree in the browser. The built-in themes `dark` and `light` come with the assets (their colors are
 * resolved from the TypeScript themes by tools/ts_export_themes.mts); custom themes are not supported. Port of generateHtml in
 * core/export-html/index.ts. Placeholders are replaced in one pass over the template, so text a replacement contains is never
 * taken for a placeholder.
 */
export class HtmlExporter {
public:
    HtmlExporter(ExportAssets assets, const IBase64Codec& base64)
        : m_assets(std::move(assets)),
          m_base64(base64) {}

    /** `themeName` empty selects `dark`. */
    Result<std::string> render(const Json& sessionData, const std::string& themeName) const {
        const Json themes = Json::parse(m_assets.themesJson, nullptr, false);
        const std::string name = themeName.empty() ? "dark" : themeName;
        if (!themes.is_object() || !themes.contains(name)) {
            return std::unexpected(Error{"unknown_theme", "Unknown theme \"" + name + "\" (available: " + available(themes) + ")"});
        }
        const Json& theme = themes[name];
        const Json& colors = theme["colors"];
        const Json& exportColors = theme["export"];
        std::string vars;
        for (const auto& entry : colors.items()) {
            vars += (vars.empty() ? "" : "\n      ") + std::string("--") + entry.key() + ": " + entry.value().get<std::string>() + ";";
        }
        const std::string pageBg = exportColors.value("pageBg", "rgb(24, 24, 30)");
        const std::string cardBg = exportColors.value("cardBg", "rgb(30, 30, 36)");
        const std::string infoBg = exportColors.value("infoBg", "rgb(60, 55, 40)");
        vars += "\n      --exportPageBg: " + pageBg + ";\n      --exportCardBg: " + cardBg + ";\n      --exportInfoBg: " + infoBg + ";";

        const std::string css = fill(m_assets.templateCss, {{"THEME_VARS", vars}, {"BODY_BG", pageBg}, {"CONTAINER_BG", cardBg}, {"INFO_BG", infoBg}});
        const std::string data = m_base64.encode(sessionData.dump(-1, ' ', false, Json::error_handler_t::replace));
        return fill(m_assets.templateHtml, {{"CSS", css}, {"JS", m_assets.templateJs}, {"SESSION_DATA", data}, {"MARKED_JS", m_assets.markedJs}, {"HIGHLIGHT_JS", m_assets.highlightJs}});
    }

private:
    /** `text` with each `{{NAME}}` (the first of each) replaced; other text, also inside replacements, stays as it is. */
    std::string fill(const std::string& text, const std::map<std::string, std::string>& values) const {
        std::string out;
        std::set<std::string> used;
        std::size_t position = 0;
        while (position < text.size()) {
            const std::size_t open = text.find("{{", position);
            if (open == std::string::npos) {
                break;
            }
            const std::size_t close = text.find("}}", open + 2);
            const std::string name = close == std::string::npos ? "" : text.substr(open + 2, close - open - 2);
            const auto found = values.find(name);
            if (found == values.end() || used.contains(name)) {
                out += text.substr(position, open + 2 - position);
                position = open + 2;
                continue;
            }
            out += text.substr(position, open - position) + found->second;
            used.insert(name);
            position = close + 2;
        }
        return out + text.substr(std::min(position, text.size()));
    }

    std::string available(const Json& themes) const {
        std::string names;
        if (themes.is_object()) {
            for (const auto& entry : themes.items()) {
                names += (names.empty() ? "" : ", ") + entry.key();
            }
        }
        return names;
    }

    ExportAssets m_assets;
    const IBase64Codec& m_base64;
};
