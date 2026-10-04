module;

#include <cstdint>

export module pi.support.timings;

import std;
export import pi.platform.i_clock;

/**
 * Startup profiling (PI_TIMING=1): `time(label)` records the milliseconds since the previous mark of a namespace, report()
 * prints the groups. Disabled instances record nothing. Port of core/timings.ts.
 */
export class Timings {
public:
    Timings(const IClock& clock, bool enabled)
        : m_clock(clock),
          m_enabled(enabled) {}

    bool enabled() const {
        return m_enabled;
    }

    void reset(const std::string& group = "main") {
        if (!m_enabled) {
            return;
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_entries[group].clear();
        m_last[group] = m_clock.nowMs();
    }

    void time(const std::string& label, const std::string& group = "main") {
        if (!m_enabled) {
            return;
        }
        const std::int64_t now = m_clock.nowMs();
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto last = m_last.find(group);
        m_entries[group].emplace_back(label, now - (last == m_last.end() ? now : last->second));
        m_last[group] = now;
    }

    /** The groups as text (empty when disabled or nothing was marked). */
    std::string report() const {
        if (!m_enabled) {
            return "";
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::string out;
        for (const auto& [group, marks] : m_entries) {
            std::int64_t total = 0;
            std::string lines;
            for (const auto& [label, ms] : marks) {
                if (ms >= 0) {
                    lines += "  " + label + ": " + std::to_string(ms) + "ms\n";
                    total += ms;
                }
            }
            if (lines.empty()) {
                continue;
            }
            const std::string title = "Startup Timings: " + group;
            out += "\n--- " + title + " ---\n" + lines + "  TOTAL: " + std::to_string(total) + "ms\n" + std::string(title.size() + 8, '-') + "\n\n";
        }
        return out;
    }

private:
    const IClock& m_clock;
    bool m_enabled;
    mutable std::mutex m_mutex;
    std::map<std::string, std::vector<std::pair<std::string, std::int64_t>>> m_entries;
    std::map<std::string, std::int64_t> m_last;
};
