export module pi.support.theme_color_parser;

import std;
import pi.support.oklab_converter;
export import pi.types.json;
export import pi.types.result;
export import pi.types.theme_color;

/**
 * Theme color values: `#rgb`, `#rrggbb`, `oklch(L C H)` (L may be a percentage), `okhsl(H S L)` (S and L may be percentages) or a
 * 256-color palette index. Port of parseColor in packages/tui/src/colors.ts and its conversions to sRGB and hex.
 */
export class ThemeColorParser {
public:
    Result<ThemeColor> parse(const Json& value) const {
        if (value.is_number_integer()) {
            return indexed(value.get<std::int64_t>());
        }
        if (!value.is_string()) {
            return std::unexpected(invalid(value.dump()));
        }
        const std::string text = value.get<std::string>();
        std::smatch match;
        if (std::regex_match(text, match, std::regex("^#([\\da-f]{3}|[\\da-f]{6})$", std::regex::icase))) {
            return hex(match[1].str());
        }
        const std::string number = "[+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:e[+-]?\\d+)?";
        if (std::regex_match(text, match, std::regex("^oklch\\(\\s*(" + number + ")(%)?\\s+(" + number + ")\\s+(" + number + ")(?:deg)?\\s*\\)$", std::regex::icase))) {
            return oklch(std::stod(match[1].str()) / (match[2].matched ? 100 : 1), std::stod(match[3].str()), std::stod(match[4].str()), text);
        }
        if (std::regex_match(text, match, std::regex("^okhsl\\(\\s*(" + number + ")(?:deg)?\\s+(" + number + ")(%)?\\s+(" + number + ")(%)?\\s*\\)$", std::regex::icase))) {
            return okhsl(std::stod(match[1].str()), std::stod(match[2].str()) / (match[3].matched ? 100 : 1), std::stod(match[4].str()) / (match[5].matched ? 100 : 1), text);
        }
        return std::unexpected(invalid(text));
    }

    /** `#rrggbb` of an sRGB color (channels rounded half up). */
    std::string toHex(const RgbColor& color) const {
        std::string out = "#";
        for (const double channel : {color.r, color.g, color.b}) {
            out += std::format("{:02x}", static_cast<int>(std::floor(channel + 0.5)));
        }
        return out;
    }

private:
    Error invalid(const std::string& text) const {
        return Error{"invalid_color", "Invalid color value: " + text};
    }

    Result<ThemeColor> indexed(std::int64_t index) const {
        if (index < 0 || index > 255) {
            return std::unexpected(Error{"invalid_color", "ANSI color index must be an integer from 0 to 255: " + std::to_string(index)});
        }
        return withLightness(paletteColor(static_cast<int>(index)), index < 16);
    }

    ThemeColor hex(const std::string& digits) const {
        const std::string full = digits.size() == 3 ? std::string{digits[0], digits[0], digits[1], digits[1], digits[2], digits[2]} : digits;
        const auto channel = [&](std::size_t at) { return static_cast<double>(std::stoi(full.substr(at, 2), nullptr, 16)); };
        return withLightness(RgbColor{channel(0), channel(2), channel(4)}, false);
    }

    Result<ThemeColor> oklch(double l, double c, double h, const std::string& text) const {
        if (!std::isfinite(l) || !std::isfinite(c) || !std::isfinite(h) || l < 0 || l > 1 || c < 0) {
            return std::unexpected(invalid(text));
        }
        ThemeColor color;
        color.rgb = m_oklab.oklchToRgb(l, c, std::fmod(std::fmod(h, 360) + 360, 360));
        color.lightness = l;
        return color;
    }

    Result<ThemeColor> okhsl(double h, double s, double l, const std::string& text) const {
        if (!std::isfinite(h) || !std::isfinite(s) || !std::isfinite(l) || s < 0 || s > 1 || l < 0 || l > 1) {
            return std::unexpected(invalid(text));
        }
        return withLightness(m_oklab.okhslToRgb(h, s, l), false);
    }

    ThemeColor withLightness(const RgbColor& rgb, bool terminalPalette) const {
        ThemeColor color;
        color.rgb = rgb;
        color.lightness = m_oklab.rgbToOklab(rgb)[0];
        color.terminalPalette = terminalPalette;
        return color;
    }

    /** The xterm 256-color palette: 16 basic colors, a 6x6x6 cube and 24 grays. */
    RgbColor paletteColor(int index) const {
        if (index < 16) {
            const std::array<std::array<int, 3>, 16> basic{std::array<int, 3>{0, 0, 0},       {128, 0, 0},     {0, 128, 0},     {128, 128, 0},
                                                           {0, 0, 128},                       {128, 0, 128},   {0, 128, 128},   {192, 192, 192},
                                                           {128, 128, 128},                   {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
                                                           {0, 0, 255},                       {255, 0, 255},   {0, 255, 255},   {255, 255, 255}};
            return RgbColor{static_cast<double>(basic[index][0]), static_cast<double>(basic[index][1]), static_cast<double>(basic[index][2])};
        }
        if (index < 232) {
            const std::array<int, 6> cube{0, 95, 135, 175, 215, 255};
            const int at = index - 16;
            return RgbColor{static_cast<double>(cube[at / 36]), static_cast<double>(cube[(at % 36) / 6]), static_cast<double>(cube[at % 6])};
        }
        const double gray = 8 + (index - 232) * 10;
        return RgbColor{gray, gray, gray};
    }

    OklabConverter m_oklab;
};
