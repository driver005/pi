export module pi.types.theme_color;

export import pi.types.rgb_color;

/** A theme color as the export needs it: its sRGB value, its Oklab lightness (0-1) and whether it is one of the 16 palette colors a terminal themes. */
export struct ThemeColor {
    RgbColor rgb;
    double lightness = 0;
    bool terminalPalette = false;
};
