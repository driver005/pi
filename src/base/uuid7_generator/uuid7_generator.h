#pragma once

#include <cstdint>
#include <mutex>
#include <random>
#include <string>

#include "interfaces/platform/i_clock/i_clock.h"
#include "interfaces/platform/i_id_generator/i_id_generator.h"

/** RFC 9562 UUIDv7 generator; ids from one instance are strictly increasing. */
class Uuid7Generator : public IIdGenerator {
public:
    explicit Uuid7Generator(const IClock& clock);

    std::string next() override;

private:
    const IClock& m_clock;
    std::mutex m_mutex;
    std::mt19937_64 m_random;
    std::int64_t m_lastMs = 0;
    std::uint16_t m_counter = 0;
};
