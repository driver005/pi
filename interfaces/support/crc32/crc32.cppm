module;

#include <cstdint>

export module pi.support.crc32;

import std;

/** The CRC-32 (IEEE 802.3, as used by ZIP and PNG) of a byte string. */
export class Crc32 {
public:
    Crc32() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) != 0 ? 0xEDB88320U ^ (value >> 1) : value >> 1;
            }
            m_table[i] = value;
        }
    }

    std::uint32_t compute(std::string_view bytes) const {
        std::uint32_t crc = 0xFFFFFFFFU;
        for (const char c : bytes) {
            crc = m_table[(crc ^ static_cast<unsigned char>(c)) & 0xFFU] ^ (crc >> 8);
        }
        return crc ^ 0xFFFFFFFFU;
    }

private:
    std::array<std::uint32_t, 256> m_table{};
};
