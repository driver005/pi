export module pi.support.short_id_generator;

import std;

/** Random 8-hex-character ids for session entries, unique against a set of taken ids. */
export class ShortIdGenerator {
public:
    ShortIdGenerator()
        : m_random(std::random_device{}()) {}

    explicit ShortIdGenerator(std::uint32_t seed)
        : m_random(seed) {}

    /** An id not in `taken`; after 100 collisions a full UUID-shaped id is returned. */
    std::string next(const std::function<bool(const std::string&)>& isTaken) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            std::string id = randomHex(8);
            if (!isTaken(id)) {
                return id;
            }
        }
        return randomHex(8) + "-" + randomHex(4) + "-4" + randomHex(3) + "-a" + randomHex(3) + "-" + randomHex(12);
    }

private:
    std::string randomHex(std::size_t digits) {
        constexpr char alphabet[] = "0123456789abcdef";
        std::uniform_int_distribution<int> pick(0, 15);
        std::string out;
        for (std::size_t i = 0; i < digits; ++i) {
            out.push_back(alphabet[pick(m_random)]);
        }
        return out;
    }

    std::mt19937 m_random;
};
