export module pi.types.image_content;

import std;

/** Inline image: base64 data plus MIME type (image/png, image/jpeg, ...). */
export struct ImageContent {
    std::string data;
    std::string mimeType;
};
