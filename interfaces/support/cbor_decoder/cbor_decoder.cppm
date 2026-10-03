module;

#include <cmath>
#include <cstdint>
#include <cstring>

export module pi.support.cbor_decoder;

import std;
export import pi.support.utf8_validator;
export import pi.types.cbor_options;
export import pi.types.json;
export import pi.types.result;

/**
 * Decodes exactly one item of the protocol's strict RFC 8949 subset (see CborEncoder): rejects
 * tags, indefinite lengths, half/single floats, simple values other than false/true/null,
 * non-string or duplicate map keys, invalid UTF-8, unsafe integers, non-finite numbers and
 * trailing data. Integer-valued float64s decode as integers (-0 stays a float), like JavaScript
 * numbers. Port of packages/protocol/src/cbor/decoder.ts.
 */
export class CborDecoder {
public:
    static constexpr std::uint64_t kMaxSafeInteger = 9007199254740991;

    Result<Json> decode(std::string_view bytes, const CborOptions& options = {}) const;

private:
    Result<Json> readItem(std::string_view bytes, std::size_t& offset, const CborOptions& options, int depth) const;
    Result<Json> readNegative(std::string_view bytes, std::size_t& offset, int info) const;
    Result<Json> readByteString(std::string_view bytes, std::size_t& offset, int info, const CborOptions& options) const;
    Result<Json> readTextString(std::string_view bytes, std::size_t& offset, int info, const CborOptions& options) const;
    Result<Json> readArray(std::string_view bytes, std::size_t& offset, int info, const CborOptions& options, int depth) const;
    Result<Json> readMap(std::string_view bytes, std::size_t& offset, int info, const CborOptions& options, int depth) const;
    Result<Json> readSimple(std::string_view bytes, std::size_t& offset, int info) const;
    Result<std::uint64_t> readLength(std::string_view bytes, std::size_t& offset, int info, const std::string& kind,
                                     std::uint64_t limit) const;
    Result<std::uint64_t> readArgument(std::string_view bytes, std::size_t& offset, int info) const;
    Result<std::string_view> readBytes(std::string_view bytes, std::size_t& offset, std::uint64_t length) const;
    Error failure(const std::string& message) const;

    Utf8Validator m_utf8;
};

Error CborDecoder::failure(const std::string& message) const {
    return Error{"cbor", message};
}

Result<std::string_view> CborDecoder::readBytes(std::string_view bytes, std::size_t& offset,
                                                std::uint64_t length) const {
    if (length > bytes.size() - offset) {
        return std::unexpected(failure("Truncated CBOR payload"));
    }
    const std::string_view view = bytes.substr(offset, length);
    offset += length;
    return view;
}

Result<std::uint64_t> CborDecoder::readArgument(std::string_view bytes, std::size_t& offset, int info) const {
    if (info < 24) {
        return static_cast<std::uint64_t>(info);
    }
    if (info > 27) {
        return std::unexpected(failure(info == 31 ? "Indefinite-length CBOR items are not supported"
                                                  : "Malformed CBOR additional information"));
    }
    const std::size_t width = info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : 8;
    auto raw = readBytes(bytes, offset, width);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    std::uint64_t value = 0;
    for (const char c : *raw) {
        value = (value << 8) | static_cast<unsigned char>(c);
    }
    if (width == 8 && value > kMaxSafeInteger) {
        return std::unexpected(failure("Decoded CBOR integer or length is outside the safe range"));
    }
    return value;
}

Result<std::uint64_t> CborDecoder::readLength(std::string_view bytes, std::size_t& offset, int info,
                                              const std::string& kind, std::uint64_t limit) const {
    if (info == 31) {
        return std::unexpected(failure("Indefinite-length CBOR " + kind + "s are not supported"));
    }
    auto length = readArgument(bytes, offset, info);
    if (!length) {
        return length;
    }
    if (*length > limit) {
        return std::unexpected(failure("CBOR " + kind + " length exceeds configured limit of " + std::to_string(limit)));
    }
    return length;
}

Result<Json> CborDecoder::readNegative(std::string_view bytes, std::size_t& offset, int info) const {
    auto argument = readArgument(bytes, offset, info);
    if (!argument) {
        return std::unexpected(argument.error());
    }
    if (*argument >= kMaxSafeInteger) {
        return std::unexpected(failure("Decoded CBOR integer is outside the safe range"));
    }
    return Json(-1 - static_cast<std::int64_t>(*argument));
}

Result<Json> CborDecoder::readByteString(std::string_view bytes, std::size_t& offset, int info,
                                         const CborOptions& options) const {
    auto length = readLength(bytes, offset, info, "byte string", options.maxByteLength);
    if (!length) {
        return std::unexpected(length.error());
    }
    auto raw = readBytes(bytes, offset, *length);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    return Json::binary(std::vector<std::uint8_t>(raw->begin(), raw->end()));
}

Result<Json> CborDecoder::readTextString(std::string_view bytes, std::size_t& offset, int info,
                                         const CborOptions& options) const {
    auto length = readLength(bytes, offset, info, "text string", options.maxByteLength);
    if (!length) {
        return std::unexpected(length.error());
    }
    auto raw = readBytes(bytes, offset, *length);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    if (!m_utf8.valid(*raw)) {
        return std::unexpected(failure("CBOR text string contains invalid UTF-8"));
    }
    return Json(std::string(*raw));
}

Result<Json> CborDecoder::readArray(std::string_view bytes, std::size_t& offset, int info,
                                    const CborOptions& options, int depth) const {
    auto length = readLength(bytes, offset, info, "array", options.maxContainerLength);
    if (!length) {
        return std::unexpected(length.error());
    }
    Json array = Json::array();
    for (std::uint64_t index = 0; index < *length; ++index) {
        auto item = readItem(bytes, offset, options, depth + 1);
        if (!item) {
            return item;
        }
        array.push_back(std::move(*item));
    }
    return array;
}

Result<Json> CborDecoder::readMap(std::string_view bytes, std::size_t& offset, int info,
                                  const CborOptions& options, int depth) const {
    auto length = readLength(bytes, offset, info, "map", options.maxContainerLength);
    if (!length) {
        return std::unexpected(length.error());
    }
    Json map = Json::object();
    for (std::uint64_t index = 0; index < *length; ++index) {
        auto key = readItem(bytes, offset, options, depth + 1);
        if (!key) {
            return key;
        }
        if (!key->is_string()) {
            return std::unexpected(failure("CBOR map keys must be strings"));
        }
        if (map.contains(key->get_ref<const std::string&>())) {
            return std::unexpected(failure("CBOR map contains a duplicate key"));
        }
        auto value = readItem(bytes, offset, options, depth + 1);
        if (!value) {
            return value;
        }
        map[key->get_ref<const std::string&>()] = std::move(*value);
    }
    return map;
}

Result<Json> CborDecoder::readSimple(std::string_view bytes, std::size_t& offset, int info) const {
    switch (info) {
    case 20:
        return Json(false);
    case 21:
        return Json(true);
    case 22:
        return Json(nullptr);
    case 27: {
        auto raw = readBytes(bytes, offset, 8);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        std::uint64_t bits = 0;
        for (const char c : *raw) {
            bits = (bits << 8) | static_cast<unsigned char>(c);
        }
        double number = 0;
        std::memcpy(&number, &bits, sizeof(number));
        if (!std::isfinite(number)) {
            return std::unexpected(failure("Decoded CBOR number must be finite"));
        }
        const bool negativeZero = number == 0.0 && std::signbit(number);
        if (std::floor(number) == number && !negativeZero) {
            if (std::fabs(number) > static_cast<double>(kMaxSafeInteger)) {
                return std::unexpected(failure("Decoded CBOR integer is outside the safe range"));
            }
            return Json(static_cast<std::int64_t>(number));
        }
        return Json(number);
    }
    case 31:
        return std::unexpected(failure("CBOR break marker is not supported"));
    default:
        return std::unexpected(failure("Unsupported CBOR simple value or floating-point width"));
    }
}

Result<Json> CborDecoder::readItem(std::string_view bytes, std::size_t& offset, const CborOptions& options,
                                   int depth) const {
    if (depth > options.maxDepth) {
        return std::unexpected(failure("CBOR nesting depth exceeds configured limit of " + std::to_string(options.maxDepth)));
    }
    if (offset >= bytes.size()) {
        return std::unexpected(failure("Truncated CBOR payload"));
    }
    const auto initial = static_cast<unsigned char>(bytes[offset++]);
    const int major = initial >> 5;
    const int info = initial & 0x1F;
    switch (major) {
    case 0: {
        auto value = readArgument(bytes, offset, info);
        if (!value) {
            return std::unexpected(value.error());
        }
        return Json(static_cast<std::int64_t>(*value));
    }
    case 1:
        return readNegative(bytes, offset, info);
    case 2:
        return readByteString(bytes, offset, info, options);
    case 3:
        return readTextString(bytes, offset, info, options);
    case 4:
        return readArray(bytes, offset, info, options, depth);
    case 5:
        return readMap(bytes, offset, info, options, depth);
    case 6:
        return std::unexpected(failure("CBOR tags are not supported"));
    default:
        return readSimple(bytes, offset, info);
    }
}

Result<Json> CborDecoder::decode(std::string_view bytes, const CborOptions& options) const {
    if (bytes.size() > options.maxByteLength) {
        return std::unexpected(failure("CBOR byte length exceeds configured limit of " + std::to_string(options.maxByteLength)));
    }
    std::size_t offset = 0;
    auto value = readItem(bytes, offset, options, 0);
    if (!value) {
        return value;
    }
    if (offset != bytes.size()) {
        return std::unexpected(failure("CBOR payload contains trailing data"));
    }
    return value;
}
