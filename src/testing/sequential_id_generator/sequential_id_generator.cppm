export module pi.testing.sequential_id_generator;

import std;
export import pi.platform.i_id_generator;

/** IIdGenerator producing "<prefix>1", "<prefix>2", ... for deterministic tests. */
export class SequentialIdGenerator : public IIdGenerator {
public:
    explicit SequentialIdGenerator(std::string prefix = "id")
        : m_prefix(std::move(prefix)) {}

    std::string next() override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_prefix + std::to_string(++m_counter);
    }

private:
    std::mutex m_mutex;
    std::string m_prefix;
    int m_counter = 0;
};
