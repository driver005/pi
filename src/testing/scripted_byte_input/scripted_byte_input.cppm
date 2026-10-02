export module pi.testing.scripted_byte_input;

import std;
export import pi.platform.i_byte_input;

/** IByteInput that yields queued chunks, then ends (or blocks until more are pushed and closed). */
export class ScriptedByteInput : public IByteInput {
public:
    void push(std::string chunk);
    /** After close, read() returns nullopt once the queue is drained. */
    void close();

    std::optional<std::string> read() override;

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<std::string> m_chunks;
    bool m_closed = false;
};

void ScriptedByteInput::push(std::string chunk) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_chunks.push_back(std::move(chunk));
    }
    m_changed.notify_all();
}

void ScriptedByteInput::close() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
    }
    m_changed.notify_all();
}

std::optional<std::string> ScriptedByteInput::read() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_changed.wait(lock, [this] { return !m_chunks.empty() || m_closed; });
    if (m_chunks.empty()) {
        return std::nullopt;
    }
    std::string chunk = std::move(m_chunks.front());
    m_chunks.pop_front();
    return chunk;
}
