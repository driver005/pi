// style:c-abi
// The files of the HTML export, embedded into the binary (tools/embed_asset.py); each is `piAsset_<name>` with its length in `piAsset_<name>_size`.
#ifndef PI_EXPORT_HTML_ASSETS_H
#define PI_EXPORT_HTML_ASSETS_H

extern const char piAsset_template_html[];
extern const unsigned long piAsset_template_html_size;
extern const char piAsset_template_css[];
extern const unsigned long piAsset_template_css_size;
extern const char piAsset_template_js[];
extern const unsigned long piAsset_template_js_size;
extern const char piAsset_marked_js[];
extern const unsigned long piAsset_marked_js_size;
extern const char piAsset_highlight_js[];
extern const unsigned long piAsset_highlight_js_size;
extern const char piAsset_export_themes_json[];
extern const unsigned long piAsset_export_themes_json_size;

#endif
