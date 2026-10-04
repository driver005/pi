module;

#include <cstdint>

export module pi.support.zip_writer;

import std;
export import pi.types.zip_entry;
import pi.support.crc32;

/**
 * Builds the small classic ZIP archives bug reports use. Entries are stored without compression (method 0), which every unzip
 * tool reads; TypeScript deflates them, the difference is only size. Names are UTF-8 (general purpose bit 11); the timestamp
 * is UTC.
 */
export class ZipWriter {
public:
    std::string create(const std::vector<ZipEntry>& entries, std::int64_t epochMs) const {
        const std::chrono::sys_seconds seconds = std::chrono::floor<std::chrono::seconds>(std::chrono::sys_time<std::chrono::milliseconds>(std::chrono::milliseconds(epochMs)));
        const auto days = std::chrono::floor<std::chrono::days>(seconds);
        const std::chrono::year_month_day date{days};
        const std::chrono::hh_mm_ss<std::chrono::seconds> clock{seconds - days};
        const auto time = static_cast<std::uint16_t>((static_cast<unsigned>(clock.hours().count()) << 11) | (static_cast<unsigned>(clock.minutes().count()) << 5) | (static_cast<unsigned>(clock.seconds().count()) >> 1));
        const int year = std::max(1980, static_cast<int>(date.year()));
        const auto day = static_cast<std::uint16_t>(((year - 1980) << 9) | (static_cast<unsigned>(date.month()) << 5) | static_cast<unsigned>(date.day()));

        std::string files;
        std::string directory;
        for (const ZipEntry& entry : entries) {
            const std::uint32_t checksum = m_crc.compute(entry.data);
            const auto size = static_cast<std::uint32_t>(entry.data.size());
            const auto offset = static_cast<std::uint32_t>(files.size());
            files += le32(0x04034b50) + le16(20) + le16(0x0800) + le16(0) + le16(time) + le16(day) + le32(checksum) + le32(size) + le32(size) + le16(static_cast<std::uint16_t>(entry.name.size())) + le16(0) + entry.name + entry.data;
            directory += le32(0x02014b50) + le16(20) + le16(20) + le16(0x0800) + le16(0) + le16(time) + le16(day) + le32(checksum) + le32(size) + le32(size) + le16(static_cast<std::uint16_t>(entry.name.size())) + le16(0) + le16(0) + le16(0) + le16(0) + le32(0) + le32(offset) + entry.name;
        }
        const auto count = static_cast<std::uint16_t>(entries.size());
        const std::string end = le32(0x06054b50) + le16(0) + le16(0) + le16(count) + le16(count) + le32(static_cast<std::uint32_t>(directory.size())) + le32(static_cast<std::uint32_t>(files.size())) + le16(0);
        return files + directory + end;
    }

private:
    std::string le16(std::uint16_t value) const {
        return std::string{static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8) & 0xFFU)};
    }

    std::string le32(std::uint32_t value) const {
        return le16(static_cast<std::uint16_t>(value & 0xFFFFU)) + le16(static_cast<std::uint16_t>(value >> 16));
    }

    Crc32 m_crc;
};
