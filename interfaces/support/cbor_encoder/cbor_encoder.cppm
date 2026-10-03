module;

#include <cmath>
#include <cstdint>
#include <cstring>

export module pi.support.cbor_encoder;

import std;
export import pi.support.utf8_validator;
export import pi.types.cbor_options;
export import pi.types.json;
export import pi.types.result;

/**
 * Encodes a Json value as the protocol's strict, definite-length RFC 8949 subset: integers within
 * the JavaScript safe range, finite float64s (integer-valued numbers other than -0 encode as
 * integers), UTF-8 text, byte strings (Json binary), arrays and string-keyed maps; no tags, no
 * half/single floats, no indefinite lengths. Port of packages/protocol/src/cbor/encoder.ts.
 */
export class CborEncoder {
public:
    static constexpr std::int64_t kMaxSafeInteger = 9007199254740991;

    Result<std::string> encode(const Json& value, const CborOptions& options = {}) const {
        std::string out;
        if (auto encoded = encodeValue(out, value, options, 0); !encoded) {
            return std::unexpected(encoded.error());
        }
        return out;
    }

private:
    Result<void> encodeValue(std::string& out, const Json& value, const CborOptions& options, int depth) const {
        if (depth > options.maxDepth) {
            return std::unexpected(failure("CBOR nesting depth exceeds configured limit of " + std::to_string(options.maxDepth)));
        }
        if (value.is_null()) {
            return append(out, std::string_view("\xF6", 1), options);
        }
        if (value.is_boolean()) {
            return append(out, std::string_view(value.get<bool>() ? "\xF5" : "\xF4", 1), options);
        }
        if (value.is_number()) {
            return encodeNumber(out, value, options);
        }
        if (value.is_string()) {
            return encodeText(out, value.get_ref<const std::string&>(), options);
        }
        if (value.is_binary()) {
            return encodeBytes(out, value.get_binary(), options);
        }
        if (value.is_array()) {
            return encodeArray(out, value, options, depth);
        }
        if (value.is_object()) {
            return encodeMap(out, value, options, depth);
        }
        return std::unexpected(failure("Unsupported CBOR value type"));
    }

    Result<void> encodeNumber(std::string& out, const Json& value, const CborOptions& options) const {
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number > static_cast<std::uint64_t>(kMaxSafeInteger)) {
                return std::unexpected(failure("CBOR integers must be safe JavaScript integers"));
            }
            return encodeInteger(out, static_cast<std::int64_t>(number), options);
        }
        if (value.is_number_integer()) {
            return encodeInteger(out, value.get<std::int64_t>(), options);
        }
        const double number = value.get<double>();
        if (!std::isfinite(number)) {
            return std::unexpected(failure("CBOR numbers must be finite"));
        }
        const bool negativeZero = number == 0.0 && std::signbit(number);
        if (std::floor(number) == number && !negativeZero) {
            if (std::fabs(number) > static_cast<double>(kMaxSafeInteger)) {
                return std::unexpected(failure("CBOR integers must be safe JavaScript integers"));
            }
            return encodeInteger(out, static_cast<std::int64_t>(number), options);
        }
        std::uint64_t bits = 0;
        std::memcpy(&bits, &number, sizeof(bits));
        std::string bytes(1, static_cast<char>(0xFB));
        for (int shift = 56; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<char>((bits >> shift) & 0xFF));
        }
        return append(out, bytes, options);
    }

    Result<void> encodeInteger(std::string& out, std::int64_t value, const CborOptions& options) const {
        if (value > kMaxSafeInteger || value < -kMaxSafeInteger) {
            return std::unexpected(failure("CBOR integers must be safe JavaScript integers"));
        }
        if (value >= 0) {
            return writeArgument(out, 0, static_cast<std::uint64_t>(value), options);
        }
        return writeArgument(out, 1, static_cast<std::uint64_t>(-1 - value), options);
    }

    Result<void> encodeText(std::string& out, const std::string& text, const CborOptions& options) const {
        if (text.size() > options.maxByteLength) {
            return std::unexpected(failure(limitMessage("text string length", options.maxByteLength)));
        }
        if (!m_utf8.valid(text)) {
            return std::unexpected(failure("CBOR text strings must contain valid Unicode scalar values"));
        }
        if (auto head = writeArgument(out, 3, text.size(), options); !head) {
            return head;
        }
        return append(out, text, options);
    }

    Result<void> encodeBytes(std::string& out, const Json::binary_t& bytes, const CborOptions& options) const {
        if (bytes.size() > options.maxByteLength) {
            return std::unexpected(failure(limitMessage("byte string length", options.maxByteLength)));
        }
        if (auto head = writeArgument(out, 2, bytes.size(), options); !head) {
            return head;
        }
        return append(out, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), options);
    }

    Result<void> encodeArray(std::string& out, const Json& value, const CborOptions& options, int depth) const {
        if (value.size() > options.maxContainerLength) {
            return std::unexpected(failure(limitMessage("array length", options.maxContainerLength)));
        }
        if (auto head = writeArgument(out, 4, value.size(), options); !head) {
            return head;
        }
        for (const Json& item : value) {
            if (auto encoded = encodeValue(out, item, options, depth + 1); !encoded) {
                return encoded;
            }
        }
        return {};
    }

    Result<void> encodeMap(std::string& out, const Json& value, const CborOptions& options, int depth) const {
        if (value.size() > options.maxContainerLength) {
            return std::unexpected(failure(limitMessage("map length", options.maxContainerLength)));
        }
        if (auto head = writeArgument(out, 5, value.size(), options); !head) {
            return head;
        }
        for (const auto& entry : value.items()) {
            if (auto key = encodeText(out, entry.key(), options); !key) {
                return key;
            }
            if (auto encoded = encodeValue(out, entry.value(), options, depth + 1); !encoded) {
                return encoded;
            }
        }
        return {};
    }

    Result<void> writeArgument(std::string& out, int majorType, std::uint64_t value, const CborOptions& options) const {
        const auto prefix = static_cast<unsigned char>(majorType << 5);
        std::string head;
        if (value < 24) {
            head.push_back(static_cast<char>(prefix | value));
        } else {
            const int extra = value <= 0xFF ? 1 : value <= 0xFFFF ? 2 : value <= 0xFFFFFFFFULL ? 4 : 8;
            const int code = extra == 1 ? 24 : extra == 2 ? 25 : extra == 4 ? 26 : 27;
            head.push_back(static_cast<char>(prefix | code));
            for (int shift = (extra - 1) * 8; shift >= 0; shift -= 8) {
                head.push_back(static_cast<char>((value >> shift) & 0xFF));
            }
        }
        return append(out, head, options);
    }

    Result<void> append(std::string& out, std::string_view bytes, const CborOptions& options) const {
        if (out.size() + bytes.size() > options.maxByteLength) {
            return std::unexpected(failure(limitMessage("byte length", options.maxByteLength)));
        }
        out.append(bytes);
        return {};
    }

    Error failure(const std::string& message) const {
        return Error{"cbor", message};
    }

    std::string limitMessage(const std::string& what, std::uint64_t limit) const {
        return "CBOR " + what + " exceeds configured limit of " + std::to_string(limit);
    }

    Utf8Validator m_utf8;
};
