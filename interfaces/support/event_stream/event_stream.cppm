export module pi.support.event_stream;

import std;

/**
 * Push queue with a terminal result, consumed by a single reader.
 * Producers call push()/end() from any thread; the consumer calls next() until it returns
 * nullopt, then result(). A pushed event for which isComplete() is true closes the stream and
 * defines the result via extractResult().
 */
export template <typename T, typename R>
class EventStream {
public:
    using CompletePredicate = std::function<bool(const T&)>;
    using ResultExtractor = std::function<R(const T&)>;

    EventStream(CompletePredicate isComplete, ResultExtractor extractResult)
        : m_isComplete(std::move(isComplete)), m_extractResult(std::move(extractResult)) {}

    void push(T event) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return;
        }
        const bool complete = m_isComplete(event);
        if (complete) {
            m_result = m_extractResult(event);
            m_closed = true;
        }
        m_queue.push_back(std::move(event));
        m_changed.notify_all();
    }

    /** Closes the stream with an explicit result (used when no terminal event was pushed). */
    void end(std::optional<R> result = std::nullopt) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_closed) {
            m_closed = true;
            if (result.has_value()) {
                m_result = std::move(result);
            }
        }
        m_changed.notify_all();
    }

    /** Blocks for the next event; nullopt once the stream is closed and drained. */
    std::optional<T> next() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return !m_queue.empty() || m_closed; });
        if (m_queue.empty()) {
            return std::nullopt;
        }
        T event = std::move(m_queue.front());
        m_queue.pop_front();
        return event;
    }

    /** Blocks until the stream is closed and returns the result (nullopt if none was set). */
    std::optional<R> result() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_closed; });
        return m_result;
    }

private:
    CompletePredicate m_isComplete;
    ResultExtractor m_extractResult;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<T> m_queue;
    bool m_closed = false;
    std::optional<R> m_result;
};
