export module pi.testing.recording_sleeper;

import std;
export import pi.platform.i_sleeper;

/** ISleeper that records requested delays and returns immediately. */
export class RecordingSleeper : public ISleeper {
public:
    /** The next sleep aborts the signal instead of waiting. */
    void abortOnNextSleep();
    std::vector<std::int64_t> delays() const;

    bool sleep(std::chrono::milliseconds duration,
               const std::shared_ptr<AbortSignal>& signal) override;

private:
    mutable std::mutex m_mutex;
    std::vector<std::int64_t> m_delays;
    bool m_abortOnNext = false;
};

void RecordingSleeper::abortOnNextSleep() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_abortOnNext = true;
}

std::vector<std::int64_t> RecordingSleeper::delays() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_delays;
}

bool RecordingSleeper::sleep(std::chrono::milliseconds duration,
                             const std::shared_ptr<AbortSignal>& signal) {
    bool abortNow = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_delays.push_back(duration.count());
        abortNow = m_abortOnNext;
        m_abortOnNext = false;
    }
    if (abortNow && signal) {
        signal->abort();
    }
    return !(signal && signal->aborted());
}
