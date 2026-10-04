module;

#include <cstdint>

export module pi.types.cbor_options;

import std;

/** Safe defaults for untrusted protocol payloads. */
export constexpr std::uint64_t kDefaultMaxCborByteLength = 16 * 1024 * 1024;
export constexpr std::uint64_t kDefaultMaxCborContainerLength = 1'000'000;
export constexpr int kDefaultMaxCborDepth = 64;

export struct CborOptions {
    /** Maximum encoded input/output bytes and maximum byte/text string length. */
    std::uint64_t maxByteLength = kDefaultMaxCborByteLength;
    /** Maximum number of elements in an array or entries in a map. */
    std::uint64_t maxContainerLength = kDefaultMaxCborContainerLength;
    /** Maximum recursive item depth. */
    int maxDepth = kDefaultMaxCborDepth;
};
