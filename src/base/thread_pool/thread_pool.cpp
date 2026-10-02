#include "src/base/thread_pool/thread_pool.h"

ThreadPool::ThreadPool(std::size_t workers) {
    for (std::size_t i = 0; i < std::max<std::size_t>(1, workers); ++i) {
        m_threads.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    for (std::thread& thread : m_threads) {
        thread.join();
    }
}

void ThreadPool::submit(std::function<void()> task) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.push_back(std::move(task));
    }
    m_wake.notify_one();
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopping || !m_tasks.empty(); });
            if (m_tasks.empty()) {
                return;
            }
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        task();
    }
}
