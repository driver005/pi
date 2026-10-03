export module pi.testing.capturing_byte_output;

import std;
export import pi.platform.i_byte_output;

/** IByteOutput that keeps everything written. */
export class CapturingByteOutput : public IByteOutput {
public:
    void write(std::string_view bytes) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_text.append(bytes);
    }

    void flush() override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        ++m_flushes;
    }

    std::string text() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_text;
    }

    int flushes() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_flushes;
    }

private:
    mutable std::mutex m_mutex;
    std::string m_text;
    int m_flushes = 0;
};
