module;

#include "export_html_assets.h"

export module pi.session.embedded_export_assets;

import std;
export import pi.session.i_export_assets;

/** The export template, libraries and theme colors compiled into the binary (assets/export_html). */
export class EmbeddedExportAssets : public IExportAssets {
public:
    ExportAssets assets() const override {
        ExportAssets out;
        out.templateHtml = std::string(piAsset_template_html, piAsset_template_html_size);
        out.templateCss = std::string(piAsset_template_css, piAsset_template_css_size);
        out.templateJs = std::string(piAsset_template_js, piAsset_template_js_size);
        out.markedJs = std::string(piAsset_marked_js, piAsset_marked_js_size);
        out.highlightJs = std::string(piAsset_highlight_js, piAsset_highlight_js_size);
        out.themesJson = std::string(piAsset_export_themes_json, piAsset_export_themes_json_size);
        return out;
    }
};
