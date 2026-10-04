export module pi.types.export_assets;

import std;

/** The files an HTML export is made of: the page template with its `{{...}}` placeholders, the libraries it inlines and the theme colors. */
export struct ExportAssets {
    std::string templateHtml;
    std::string templateCss;
    std::string templateJs;
    std::string markedJs;
    std::string highlightJs;
    /** `{"<theme>": {"appearance", "colors": {token: css color}, "export": {pageBg, cardBg, infoBg}}}`. */
    std::string themesJson;
};
