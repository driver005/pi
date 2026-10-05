export module pi.support.oklab_converter;

import std;
export import pi.types.rgb_color;

/**
 * Oklab, OKHSL and OKLCH to and from sRGB, the color math the theme colors of the HTML export need. A port of
 * packages/tui/src/oklab.ts (Bjoern Ottosson's reference implementation, https://bottosson.github.io/posts/colorpicker/,
 * Copyright (c) 2021 Bjoern Ottosson, MIT license) and of the OKLCH gamut mapping of colors.ts: out-of-gamut colors keep their
 * hue and lose chroma. JavaScript's `Math.round` rounds halves up, so rounding here does too.
 */
export class OklabConverter {
public:
    using Vector = std::array<double, 3>;
    using Matrix = std::array<Vector, 3>;

    /** Oklab [L, a, b] of sRGB channels (0-255). */
    Vector rgbToOklab(const RgbColor& color) const {
        const Vector linear{srgbToLinear(color.r / 255), srgbToLinear(color.g / 255), srgbToLinear(color.b / 255)};
        Vector lms = multiply(linearSrgbToLms(), linear);
        for (double& value : lms) {
            value = std::cbrt(value);
        }
        return multiply(lmsToLab(), lms);
    }

    /** Linear sRGB (0-1, may leave the gamut) of Oklab [L, a, b]. */
    Vector oklabToLinearSrgb(const Vector& lab) const {
        Vector lms = multiply(labToLms(), lab);
        for (double& value : lms) {
            value = value * value * value;
        }
        return multiply(lmsToLinearSrgb(), lms);
    }

    /** sRGB channels (0-255, rounded) of linear sRGB, clipping out-of-gamut channels. */
    RgbColor linearSrgbToRgb(const Vector& linear) const {
        const auto channel = [&](double value) { return round(std::min(1.0, std::max(0.0, linearToSrgb(value))) * 255); };
        return RgbColor{channel(linear[0]), channel(linear[1]), channel(linear[2])};
    }

    /** sRGB of an OKHSL color: hue in degrees, saturation and lightness 0-1. */
    RgbColor okhslToRgb(double hue, double saturation, double lightness) const {
        const double L = okhslToOklabLightness(lightness);
        Vector lab{L, 0, 0};
        if (L > 0 && L < 1 && saturation > 0) {
            const double angle = (2 * std::numbers::pi * (std::fmod(std::fmod(hue, 360) + 360, 360))) / 360;
            const double a = std::cos(angle);
            const double b = std::sin(angle);
            const Vector stops = chromaStops(L, a, b);
            const double c0 = stops[0];
            const double cMid = stops[1];
            const double cMax = stops[2];
            double chroma = 0;
            if (saturation < 0.8) {
                const double t = 1.25 * saturation;
                const double k1 = 0.8 * c0;
                chroma = (t * k1) / (1 - (1 - k1 / cMid) * t);
            } else {
                const double t = 5 * (saturation - 0.8);
                const double k1 = (0.2 * cMid * cMid * 1.25 * 1.25) / c0;
                chroma = cMid + (t * k1) / (1 - (1 - k1 / (cMax - cMid)) * t);
            }
            lab = Vector{L, chroma * a, chroma * b};
        }
        return linearSrgbToRgb(oklabToLinearSrgb(lab));
    }

    /** sRGB of an OKLCH color (lightness 0-1, chroma, hue in degrees), reducing chroma until it fits the gamut. */
    RgbColor oklchToRgb(double l, double c, double h) const {
        const double radians = (h * std::numbers::pi) / 180;
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        const auto atChroma = [&](double chroma) { return oklabToLinearSrgb(Vector{l, chroma * cosine, chroma * sine}); };
        const Vector direct = atChroma(c);
        if (inGamut(direct)) {
            return linearSrgbToRgb(direct);
        }
        Vector linear = atChroma(0);
        double low = 0;
        double high = c;
        for (int index = 0; index < 20; ++index) {
            const double chroma = (low + high) / 2;
            const Vector candidate = atChroma(chroma);
            if (inGamut(candidate)) {
                low = chroma;
                linear = candidate;
            } else {
                high = chroma;
            }
        }
        return linearSrgbToRgb(linear);
    }

private:
    /** `Math.round`: halves round toward positive infinity. */
    double round(double value) const {
        return std::floor(value + 0.5);
    }

    Vector multiply(const Matrix& matrix, const Vector& v) const {
        Vector out{};
        for (std::size_t row = 0; row < 3; ++row) {
            out[row] = matrix[row][0] * v[0] + matrix[row][1] * v[1] + matrix[row][2] * v[2];
        }
        return out;
    }

    Matrix linearSrgbToLms() const {
        return Matrix{Vector{0.4122214694707629, 0.5363325372617349, 0.0514459932675022},
                      Vector{0.2119034958178251, 0.6806995506452344, 0.1073969535369405},
                      Vector{0.0883024591900564, 0.2817188391361215, 0.6299787016738222}};
    }

    Matrix lmsToLab() const {
        return Matrix{Vector{0.210454268309314, 0.793617774702305, -0.0040720430116193},
                      Vector{1.9779985324311684, -2.42859224204858, 0.450593709617411},
                      Vector{0.0259040424655478, 0.7827717124575296, -0.8086757549230774}};
    }

    Matrix labToLms() const {
        return Matrix{Vector{1, 0.3963377773761749, 0.2158037573099136}, Vector{1, -0.1055613458156586, -0.0638541728258133},
                      Vector{1, -0.0894841775298119, -1.2914855480194092}};
    }

    Matrix lmsToLinearSrgb() const {
        return Matrix{Vector{4.0767416360759583, -3.3077115392580629, 0.2309699031821043},
                      Vector{-1.2684379732850315, 2.6097573492876882, -0.341319376002657},
                      Vector{-0.0041960761386756, -0.7034186179359362, 1.7076146940746117}};
    }

    double k1() const {
        return 0.206;
    }

    double k2() const {
        return 0.03;
    }

    double k3() const {
        return (1 + k1()) / (1 + k2());
    }

    double okhslToOklabLightness(double x) const {
        return (x * x + k1() * x) / (k3() * (x + k2()));
    }

    double linearToSrgb(double value) const {
        return value > 0.0031308 ? 1.055 * std::pow(value, 1 / 2.4) - 0.055 : 12.92 * value;
    }

    double srgbToLinear(double value) const {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    }

    bool inGamut(const Vector& linear) const {
        const double epsilon = 1e-7;
        return std::ranges::all_of(linear, [&](double channel) { return channel >= -epsilon && channel <= 1 + epsilon; });
    }

    /** Rate of change of each cube-root LMS component along a chroma direction (a, b). */
    Vector lmsSlopes(double a, double b) const {
        const Matrix rows = labToLms();
        return Vector{rows[0][1] * a + rows[0][2] * b, rows[1][1] * a + rows[1][2] * b, rows[2][1] * a + rows[2][2] * b};
    }

    /** Largest saturation (C/L) inside sRGB for hue (a, b): polynomial fit plus one Halley step. */
    double maxSaturation(double a, double b) const {
        std::size_t channel = 2;
        if (-1.8817031 * a + -0.80936501 * b > 1) {
            channel = 0;
        } else if (1.8144408 * a + -1.19445267 * b > 1) {
            channel = 1;
        }
        const std::array<std::array<double, 5>, 3> fit{std::array<double, 5>{1.19086277, 1.76576728, 0.59662641, 0.75515197, 0.56771245},
                                                       std::array<double, 5>{0.73956515, -0.45954404, 0.08285427, 0.12541073, -0.14503204},
                                                       std::array<double, 5>{1.35733652, -0.00915799, -1.1513021, -0.50559606, 0.00692167}};
        const std::array<double, 5>& k = fit[channel];
        const Vector weights = lmsToLinearSrgb()[channel];
        const double saturation = k[0] + k[1] * a + k[2] * b + k[3] * a * a + k[4] * a * b;

        const Vector slopes = lmsSlopes(a, b);
        Vector base{};
        for (std::size_t i = 0; i < 3; ++i) {
            base[i] = 1 + saturation * slopes[i];
        }
        double f = 0;
        double f1 = 0;
        double f2 = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            f += weights[i] * std::pow(base[i], 3);
            f1 += weights[i] * (3 * slopes[i] * std::pow(base[i], 2));
            f2 += weights[i] * (6 * slopes[i] * slopes[i] * base[i]);
        }
        return saturation - (f * f1) / (f1 * f1 - 0.5 * f * f2);
    }

    /** Oklab lightness and chroma of the most saturated sRGB color of hue (a, b). */
    std::array<double, 2> cusp(double a, double b) const {
        const double saturation = maxSaturation(a, b);
        const Vector linear = oklabToLinearSrgb(Vector{1, saturation * a, saturation * b});
        const double lightness = std::cbrt(1 / std::max({linear[0], linear[1], linear[2]}));
        return {lightness, lightness * saturation};
    }

    /** Chroma where the constant-lightness line at `lightness` leaves the sRGB gamut. */
    double maxChroma(double a, double b, double lightness, const std::array<double, 2>& peak) const {
        const double cuspL = peak[0];
        const double cuspC = peak[1];
        if (lightness <= cuspL) {
            return (cuspC * lightness) / cuspL;
        }
        const double t = (cuspC * (lightness - 1)) / (cuspL - 1);
        const Vector slopes = lmsSlopes(a, b);
        Vector lms{};
        Vector cubes{};
        Vector first{};
        Vector second{};
        for (std::size_t i = 0; i < 3; ++i) {
            lms[i] = lightness + t * slopes[i];
            cubes[i] = std::pow(lms[i], 3);
            first[i] = 3 * slopes[i] * std::pow(lms[i], 2);
            second[i] = 6 * slopes[i] * slopes[i] * lms[i];
        }
        const Matrix rows = lmsToLinearSrgb();
        double best = std::numeric_limits<double>::max();
        for (const Vector& row : rows) {
            const double f = dot(row, cubes) - 1;
            const double f1 = dot(row, first);
            const double f2 = dot(row, second);
            const double u = f1 / (f1 * f1 - 0.5 * f * f2);
            best = std::min(best, u >= 0 ? -f * u : std::numeric_limits<double>::max());
        }
        return t + best;
    }

    double dot(const Vector& row, const Vector& values) const {
        return row[0] * values[0] + row[1] * values[1] + row[2] * values[2];
    }

    /** OKHSL's chroma reference points at lightness L and hue (a, b): [c0, cMid, cMax]. */
    Vector chromaStops(double L, double a, double b) const {
        const std::array<double, 2> peak = cusp(a, b);
        const double cMax = maxChroma(a, b, L, peak);
        const double k = cMax / std::min(L * (peak[1] / peak[0]), (1 - L) * (peak[1] / (1 - peak[0])));
        const double midS = 0.11516993 + 1 / (7.4477897 + 4.1590124 * b +
                                              a * (-2.19557347 + 1.75198401 * b +
                                                   a * (-2.13704948 - 10.02301043 * b + a * (-4.24894561 + 5.38770819 * b + 4.69891013 * a))));
        const double midT = 0.11239642 + 1 / (1.6132032 - 0.68124379 * b +
                                              a * (0.40370612 + 0.90148123 * b +
                                                   a * (-0.27087943 + 0.6122399 * b + a * (0.00299215 - 0.45399568 * b - 0.14661872 * a))));
        const double cMid = 0.9 * k * std::sqrt(std::sqrt(1 / (1 / std::pow(L * midS, 4) + 1 / std::pow((1 - L) * midT, 4))));
        const double c0 = std::sqrt(1 / (1 / std::pow(L * 0.4, 2) + 1 / std::pow((1 - L) * 0.8, 2)));
        return Vector{c0, cMid, cMax};
    }
};
