export module pi.testing.capturing_byte_output;

import std;
export import pi.platform.i_byte_output;

/** IByteOutput that keeps everything written. */
export class CapturingByteOutput : public IByteOutput {
public:
    void write(std::string_view bytes) override;
    void flush() override;

    std::string text() const;
    int flushes() const;

private:
    mutable std::mutex m_mutex;
    std::string m_text;
    int m_flushes = 0;
};

void CapturingByteOutput::write(std::string_view bytes) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_text.append(bytes);
}

void CapturingByteOutput::flush() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_flushes;
}

std::string CapturingByteOutput::text() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_text;
}

int CapturingByteOutput::flushes() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_flushes;
}
