#include "src/base/uuid7_generator/uuid7_generator.h"

#include <cstdio>

Uuid7Generator::Uuid7Generator(const IClock& clock) : m_clock(clock) {
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device()};
    m_random.seed(seed);
}

std::string Uuid7Generator::next() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::int64_t ms = m_clock.nowMs();
    if (ms <= m_lastMs) {
        ms = m_lastMs;
        ++m_counter;
        if (m_counter > 0x0FFF) {
            ++ms;
            m_counter = 0;
        }
    } else {
        m_counter = static_cast<std::uint16_t>(m_random() & 0x01FF);
    }
    m_lastMs = ms;
    const std::uint64_t tail = m_random();
    const auto high = static_cast<std::uint32_t>(static_cast<std::uint64_t>(ms) >> 16);
    const auto mid = static_cast<std::uint16_t>(static_cast<std::uint64_t>(ms) & 0xFFFF);
    const auto version = static_cast<std::uint16_t>(0x7000 | m_counter);
    const auto variant = static_cast<std::uint16_t>(0x8000 | ((tail >> 48) & 0x3FFF));
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%08x-%04x-%04x-%04x-%012llx", high, mid, version, variant,
                  static_cast<unsigned long long>(tail & 0xFFFFFFFFFFFFULL));
    return buffer;
}
