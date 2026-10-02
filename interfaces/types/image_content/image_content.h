#pragma once

#include <string>

/** Inline image: base64 data plus MIME type (image/png, image/jpeg, ...). */
struct ImageContent {
    std::string data;
    std::string mimeType;
};
