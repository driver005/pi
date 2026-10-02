export module pi.testing.sequential_id_generator;

import std;
export import pi.platform.i_id_generator;

/** IIdGenerator producing "<prefix>1", "<prefix>2", ... for deterministic tests. */
export class SequentialIdGenerator : public IIdGenerator {
public:
    explicit SequentialIdGenerator(std::string prefix = "id");

    std::string next() override;

private:
    std::mutex m_mutex;
    std::string m_prefix;
    int m_counter = 0;
};

SequentialIdGenerator::SequentialIdGenerator(std::string prefix) : m_prefix(std::move(prefix)) {}

std::string SequentialIdGenerator::next() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_prefix + std::to_string(++m_counter);
}
