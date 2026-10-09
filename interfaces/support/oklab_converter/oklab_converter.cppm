export module pi.support.oklab_converter;

import std;
export import pi.types.rgb_color;

/**
 * Oklab, OKHSL and OKLCH to and from sRGB, the color math the theme colors of the HTML export need.
 * A port of packages/tui/src/oklab.ts (Bjoern Ottosson's reference implementation,
 * https://bottosson.github.io/posts/colorpicker/, Copyright (c) 2021 Bjoern Ottosson, MIT license)
 * and of the OKLCH gamut mapping of colors.ts: out-of-gamut colors keep their hue and lose chroma.
 * The output is checked against the TypeScript implementation (ts_theme_golden), so the arithmetic
 * keeps the order of the original. JavaScript's `Math.round` rounds halves up, so rounding here
 * does too.
 */
export class OklabConverter {
public:
    using Triplet = std::array<double, 3>;
    using Matrix = std::array<Triplet, 3>;

    /** Oklab [lightness, a, b] of sRGB channels (0-255). */
    Triplet rgbToOklab(const RgbColor& color) const {
        const Triplet linear{srgbToLinear(color.r / 255), srgbToLinear(color.g / 255),
                             srgbToLinear(color.b / 255)};
        Triplet lms = multiply(kLinearSrgbToLms, linear);
        for (double& value : lms) {
            value = std::cbrt(value);
        }
        return multiply(kLmsToLab, lms);
    }

    /** Linear sRGB (0-1, may leave the gamut) of Oklab [lightness, a, b]. */
    Triplet oklabToLinearSrgb(const Triplet& lab) const {
        Triplet lms = multiply(kLabToLms, lab);
        for (double& value : lms) {
            value = value * value * value;
        }
        return multiply(kLmsToLinearSrgb, lms);
    }

    /** sRGB channels (0-255, rounded) of linear sRGB, clipping out-of-gamut channels. */
    RgbColor linearSrgbToRgb(const Triplet& linear) const {
        const auto channel = [&](double value) {
            return roundHalfUp(std::clamp(linearToSrgb(value), 0.0, 1.0) * 255);
        };
        return RgbColor{channel(linear[0]), channel(linear[1]), channel(linear[2])};
    }

    /** sRGB of an OKHSL color: hue in degrees, saturation and lightness 0-1. */
    RgbColor okhslToRgb(double hue, double saturation, double lightness) const {
        const double oklabLightness = okhslToOklabLightness(lightness);
        Triplet lab{oklabLightness, 0, 0};
        if (oklabLightness > 0 && oklabLightness < 1 && saturation > 0) {
            const double degrees = std::fmod(std::fmod(hue, 360) + 360, 360);
            const double angle = (2 * std::numbers::pi * degrees) / 360;
            const double hueA = std::cos(angle);
            const double hueB = std::sin(angle);
            const double chroma = chromaAtSaturation(oklabLightness, hueA, hueB, saturation);
            lab = Triplet{oklabLightness, chroma * hueA, chroma * hueB};
        }
        return linearSrgbToRgb(oklabToLinearSrgb(lab));
    }

    /**
     * sRGB of an OKLCH color (lightness 0-1, chroma, hue in degrees), reducing chroma until it
     * fits the gamut.
     */
    RgbColor oklchToRgb(double lightness, double chroma, double hue) const {
        const double radians = (hue * std::numbers::pi) / 180;
        const double hueA = std::cos(radians);
        const double hueB = std::sin(radians);
        const auto atChroma = [&](double value) {
            return oklabToLinearSrgb(Triplet{lightness, value * hueA, value * hueB});
        };
        const Triplet direct = atChroma(chroma);
        if (inGamut(direct)) {
            return linearSrgbToRgb(direct);
        }
        Triplet linear = atChroma(0);
        double low = 0;
        double high = chroma;
        for (int step = 0; step < 20; ++step) {
            const double middle = (low + high) / 2;
            const Triplet candidate = atChroma(middle);
            if (inGamut(candidate)) {
                low = middle;
                linear = candidate;
            } else {
                high = middle;
            }
        }
        return linearSrgbToRgb(linear);
    }

private:
    static constexpr Matrix kLinearSrgbToLms{
        Triplet{0.4122214694707629, 0.5363325372617349, 0.0514459932675022},
        Triplet{0.2119034958178251, 0.6806995506452344, 0.1073969535369405},
        Triplet{0.0883024591900564, 0.2817188391361215, 0.6299787016738222}};
    static constexpr Matrix kLmsToLab{
        Triplet{0.210454268309314, 0.793617774702305, -0.0040720430116193},
        Triplet{1.9779985324311684, -2.42859224204858, 0.450593709617411},
        Triplet{0.0259040424655478, 0.7827717124575296, -0.8086757549230774}};
    static constexpr Matrix kLabToLms{Triplet{1, 0.3963377773761749, 0.2158037573099136},
                                      Triplet{1, -0.1055613458156586, -0.0638541728258133},
                                      Triplet{1, -0.0894841775298119, -1.2914855480194092}};
    static constexpr Matrix kLmsToLinearSrgb{
        Triplet{4.0767416360759583, -3.3077115392580629, 0.2309699031821043},
        Triplet{-1.2684379732850315, 2.6097573492876882, -0.341319376002657},
        Triplet{-0.0041960761386756, -0.7034186179359362, 1.7076146940746117}};
    /** Per sRGB channel: the polynomial fit of the maximum saturation where that channel clips
     * first. */
    static constexpr std::array<std::array<double, 5>, 3> kSaturationFit{
        std::array<double, 5>{1.19086277, 1.76576728, 0.59662641, 0.75515197, 0.56771245},
        std::array<double, 5>{0.73956515, -0.45954404, 0.08285427, 0.12541073, -0.14503204},
        std::array<double, 5>{1.35733652, -0.00915799, -1.1513021, -0.50559606, 0.00692167}};
    static constexpr double kK1 = 0.206;
    static constexpr double kK2 = 0.03;
    static constexpr double kK3 = (1 + kK1) / (1 + kK2);

    /** `Math.round`: halves round toward positive infinity. */
    double roundHalfUp(double value) const {
        return std::floor(value + 0.5);
    }

    Triplet multiply(const Matrix& matrix, const Triplet& values) const {
        Triplet out{};
        for (std::size_t row = 0; row < 3; ++row) {
            out[row] = dot(matrix[row], values);
        }
        return out;
    }

    double dot(const Triplet& row, const Triplet& values) const {
        return row[0] * values[0] + row[1] * values[1] + row[2] * values[2];
    }

    double okhslToOklabLightness(double lightness) const {
        return (lightness * lightness + kK1 * lightness) / (kK3 * (lightness + kK2));
    }

    double linearToSrgb(double value) const {
        return value > 0.0031308 ? 1.055 * std::pow(value, 1 / 2.4) - 0.055 : 12.92 * value;
    }

    double srgbToLinear(double value) const {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    }

    bool inGamut(const Triplet& linear) const {
        const double epsilon = 1e-7;
        return std::ranges::all_of(
            linear, [&](double channel) { return channel >= -epsilon && channel <= 1 + epsilon; });
    }

    /** Rate of change of each cube-root LMS component along a chroma direction (hueA, hueB). */
    Triplet lmsSlopes(double hueA, double hueB) const {
        return Triplet{kLabToLms[0][1] * hueA + kLabToLms[0][2] * hueB,
                       kLabToLms[1][1] * hueA + kLabToLms[1][2] * hueB,
                       kLabToLms[2][1] * hueA + kLabToLms[2][2] * hueB};
    }

    /** Largest saturation (chroma / lightness) inside sRGB for a hue: polynomial fit plus one
     * Halley step. */
    double maxSaturation(double hueA, double hueB) const {
        std::size_t channel = 2;
        if (-1.8817031 * hueA + -0.80936501 * hueB > 1) {
            channel = 0;
        } else if (1.8144408 * hueA + -1.19445267 * hueB > 1) {
            channel = 1;
        }
        const std::array<double, 5>& fit = kSaturationFit[channel];
        const Triplet& weights = kLmsToLinearSrgb[channel];
        const double saturation =
            fit[0] + fit[1] * hueA + fit[2] * hueB + fit[3] * hueA * hueA + fit[4] * hueA * hueB;
        const Triplet slopes = lmsSlopes(hueA, hueB);
        Triplet base{};
        for (std::size_t i = 0; i < 3; ++i) {
            base[i] = 1 + saturation * slopes[i];
        }
        double value = 0;
        double first = 0;
        double second = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            value += weights[i] * std::pow(base[i], 3);
            first += weights[i] * (3 * slopes[i] * std::pow(base[i], 2));
            second += weights[i] * (6 * slopes[i] * slopes[i] * base[i]);
        }
        return saturation - (value * first) / (first * first - 0.5 * value * second);
    }

    /** Oklab lightness and chroma of the most saturated sRGB color of a hue. */
    std::array<double, 2> cusp(double hueA, double hueB) const {
        const double saturation = maxSaturation(hueA, hueB);
        const Triplet linear = oklabToLinearSrgb(Triplet{1, saturation * hueA, saturation * hueB});
        const double lightness = std::cbrt(1 / std::max({linear[0], linear[1], linear[2]}));
        return {lightness, lightness * saturation};
    }

    /** Chroma where the constant-lightness line at `lightness` leaves the sRGB gamut. */
    double maxChroma(double hueA, double hueB, double lightness,
                     const std::array<double, 2>& peak) const {
        const double cuspLightness = peak[0];
        const double cuspChroma = peak[1];
        if (lightness <= cuspLightness) {
            return (cuspChroma * lightness) / cuspLightness;
        }
        const double edge = (cuspChroma * (lightness - 1)) / (cuspLightness - 1);
        const Triplet slopes = lmsSlopes(hueA, hueB);
        Triplet lms{};
        Triplet cubes{};
        Triplet first{};
        Triplet second{};
        for (std::size_t i = 0; i < 3; ++i) {
            lms[i] = lightness + edge * slopes[i];
            cubes[i] = std::pow(lms[i], 3);
            first[i] = 3 * slopes[i] * std::pow(lms[i], 2);
            second[i] = 6 * slopes[i] * slopes[i] * lms[i];
        }
        double best = std::numeric_limits<double>::max();
        for (const Triplet& row : kLmsToLinearSrgb) {
            const double value = dot(row, cubes) - 1;
            const double slope = dot(row, first);
            const double curvature = dot(row, second);
            const double step = slope / (slope * slope - 0.5 * value * curvature);
            best = std::min(best, step >= 0 ? -value * step : std::numeric_limits<double>::max());
        }
        return edge + best;
    }

    /** OKHSL's chroma reference points at an Oklab lightness and hue: [c0, cMid, cMax]. */
    Triplet chromaStops(double lightness, double hueA, double hueB) const {
        const std::array<double, 2> peak = cusp(hueA, hueB);
        const double cMax = maxChroma(hueA, hueB, lightness, peak);
        const double scale = cMax / std::min(lightness * (peak[1] / peak[0]),
                                             (1 - lightness) * (peak[1] / (1 - peak[0])));
        const double midS =
            0.11516993 +
            1 / (7.4477897 + 4.1590124 * hueB +
                 hueA * (-2.19557347 + 1.75198401 * hueB +
                         hueA * (-2.13704948 - 10.02301043 * hueB +
                                 hueA * (-4.24894561 + 5.38770819 * hueB + 4.69891013 * hueA))));
        const double midT =
            0.11239642 +
            1 / (1.6132032 - 0.68124379 * hueB +
                 hueA * (0.40370612 + 0.90148123 * hueB +
                         hueA * (-0.27087943 + 0.6122399 * hueB +
                                 hueA * (0.00299215 - 0.45399568 * hueB - 0.14661872 * hueA))));
        const double cMid = 0.9 * scale *
                            std::sqrt(std::sqrt(1 / (1 / std::pow(lightness * midS, 4) +
                                                     1 / std::pow((1 - lightness) * midT, 4))));
        const double c0 = std::sqrt(
            1 / (1 / std::pow(lightness * 0.4, 2) + 1 / std::pow((1 - lightness) * 0.8, 2)));
        return Triplet{c0, cMid, cMax};
    }

    /** Chroma rises from 0 through cMid at saturation 0.8 to cMax at saturation 1. */
    double chromaAtSaturation(double lightness, double hueA, double hueB, double saturation) const {
        const Triplet stops = chromaStops(lightness, hueA, hueB);
        const double c0 = stops[0];
        const double cMid = stops[1];
        const double cMax = stops[2];
        if (saturation < 0.8) {
            const double t = 1.25 * saturation;
            const double k1 = 0.8 * c0;
            return (t * k1) / (1 - (1 - k1 / cMid) * t);
        }
        const double t = 5 * (saturation - 0.8);
        const double k1 = (0.2 * cMid * cMid * 1.25 * 1.25) / c0;
        return cMid + (t * k1) / (1 - (1 - k1 / (cMax - cMid)) * t);
    }
};
