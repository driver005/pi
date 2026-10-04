export module pi.platform.i_base64_codec;

import std;

/** Base64 in the standard and URL-safe (unpadded) alphabets. */
export class IBase64Codec {
public:
    virtual ~IBase64Codec() = default;

    virtual std::string encode(std::string_view bytes) const = 0;
    virtual std::string encodeUrl(std::string_view bytes) const = 0;
    /** Accepts either alphabet, with or without padding; nullopt on invalid input. */
    virtual std::optional<std::string> decode(std::string_view text) const = 0;
};
