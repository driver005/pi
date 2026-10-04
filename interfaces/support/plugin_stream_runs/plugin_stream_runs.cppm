export module pi.support.plugin_stream_runs;

import std;
export import pi.support.abort_signal;

/**
 * The streams a plugin provider is running. close() refuses new streams, cancels the running ones and waits until every one has
 * ended, so a plugin can be unloaded without code of it still executing.
 */
export class PluginStreamRuns {
public:
    /** Records a stream that is about to run; false once closed. */
    bool begin(const std::shared_ptr<AbortSignal>& abort) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return false;
        }
        m_running.push_back(abort);
        return true;
    }

    void end(const std::shared_ptr<AbortSignal>& abort) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::erase(m_running, abort);
        m_changed.notify_all();
    }

    void close() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_closed = true;
        const std::vector<std::shared_ptr<AbortSignal>> running = m_running;
        lock.unlock();
        for (const std::shared_ptr<AbortSignal>& abort : running) {
            abort->abort();
        }
        lock.lock();
        m_changed.wait(lock, [this] { return m_running.empty(); });
    }

    std::size_t running() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_running.size();
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<std::shared_ptr<AbortSignal>> m_running;
    bool m_closed = false;
};
