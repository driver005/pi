module;

#include <cerrno>
#include <unistd.h>

export module pi.base.stdio_byte_input;

import std;
export import pi.platform.i_byte_input;

/** IByteInput over a file descriptor (stdin by default), read with read(2). */
export class StdioByteInput : public IByteInput {
public:
    explicit StdioByteInput(int fd = 0)
        : m_fd(fd) {}

    std::optional<std::string> read() override {
        char buffer[65536];
        while (true) {
            const ssize_t count = ::read(m_fd, buffer, sizeof(buffer));
            if (count > 0) {
                return std::string(buffer, static_cast<std::size_t>(count));
            }
            if (count == 0 || errno != EINTR) {
                return std::nullopt;
            }
        }
    }

private:
    int m_fd;
};
