module;

#include <cerrno>
#include <unistd.h>

export module pi.base.stdio_byte_output;

import std;
export import pi.platform.i_byte_output;

/** IByteOutput over a file descriptor (stdout by default); writes are serialized and complete. */
export class StdioByteOutput : public IByteOutput {
public:
    explicit StdioByteOutput(int fd = 1)
        : m_fd(fd) {}

    void write(std::string_view bytes) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        while (!bytes.empty()) {
            const ssize_t count = ::write(m_fd, bytes.data(), bytes.size());
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
    }
    void flush() override {}

private:
    int m_fd;
    std::mutex m_mutex;
};
