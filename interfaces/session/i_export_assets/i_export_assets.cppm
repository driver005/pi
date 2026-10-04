export module pi.session.i_export_assets;

import std;
export import pi.types.export_assets;

/** Where the HTML export finds its template, libraries and theme colors. */
export class IExportAssets {
public:
    virtual ~IExportAssets() = default;

    virtual ExportAssets assets() const = 0;
};
