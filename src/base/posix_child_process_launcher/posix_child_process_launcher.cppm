module;

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
extern char** environ;

export module pi.base.posix_child_process_launcher;

import std;
export import pi.platform.i_child_process;
export import pi.platform.i_child_process_launcher;

/** IChildProcess over three pipes and a posix_spawn'ed child in its own process group. */
export class PosixChildProcess : public IChildProcess {
public:
    PosixChildProcess(pid_t pid, int stdinFd, int stdoutFd, int stderrFd);
    ~PosixChildProcess() override;

    Result<void> write(std::string_view bytes) override;
    void closeStdin() override;
    std::optional<std::string> readOutput() override;
    std::optional<std::string> readError() override;
    std::optional<int> waitForExit(std::chrono::milliseconds timeout) override;
    void terminate() override;
    void kill() override;

private:
    std::optional<std::string> readFrom(int fd);
    void signalGroup(int signalNumber);
    bool reap(bool block);

    pid_t m_pid;
    std::mutex m_mutex;
    int m_stdin;
    int m_stdout;
    int m_stderr;
    std::optional<int> m_exitCode;
};

/** IChildProcessLauncher using posix_spawn. */
export class PosixChildProcessLauncher : public IChildProcessLauncher {
public:
    PosixChildProcessLauncher();

    Result<std::unique_ptr<IChildProcess>> launch(const ProcessRequest& request) override;

private:
    std::vector<std::string> environmentOf(const ProcessRequest& request) const;
    Result<pid_t> spawn(const ProcessRequest& request, int stdinRead, int stdoutWrite, int stderrWrite) const;
};

PosixChildProcess::PosixChildProcess(pid_t pid, int stdinFd, int stdoutFd, int stderrFd)
    : m_pid(pid), m_stdin(stdinFd), m_stdout(stdoutFd), m_stderr(stderrFd) {}

PosixChildProcess::~PosixChildProcess() {
    closeStdin();
    if (!m_exitCode) {
        signalGroup(SIGTERM);
        if (!waitForExit(std::chrono::milliseconds(500))) {
            signalGroup(SIGKILL);
            reap(true);
        }
    }
    ::close(m_stdout);
    ::close(m_stderr);
}

void PosixChildProcess::signalGroup(int signalNumber) {
    if (::kill(-m_pid, signalNumber) != 0) {
        ::kill(m_pid, signalNumber);
    }
}

bool PosixChildProcess::reap(bool block) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_exitCode) {
        return true;
    }
    int status = 0;
    pid_t result = 0;
    do {
        result = ::waitpid(m_pid, &status, block ? 0 : WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        return false;
    }
    if (result < 0) {
        m_exitCode = -1;
    } else {
        m_exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return true;
}

Result<void> PosixChildProcess::write(std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        int fd;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            fd = m_stdin;
        }
        if (fd < 0) {
            return std::unexpected(Error{"closed", "The child's stdin is closed"});
        }
        const ssize_t count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(Error{"write_failed", std::strerror(errno)});
        }
        offset += static_cast<std::size_t>(count);
    }
    return {};
}

void PosixChildProcess::closeStdin() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stdin >= 0) {
        ::close(m_stdin);
        m_stdin = -1;
    }
}

std::optional<std::string> PosixChildProcess::readFrom(int fd) {
    char buffer[65536];
    while (true) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            return std::string(buffer, static_cast<std::size_t>(count));
        }
        if (count == 0 || errno != EINTR) {
            return std::nullopt;
        }
    }
}

std::optional<std::string> PosixChildProcess::readOutput() {
    return readFrom(m_stdout);
}

std::optional<std::string> PosixChildProcess::readError() {
    return readFrom(m_stderr);
}

std::optional<int> PosixChildProcess::waitForExit(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!reap(false)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return m_exitCode;
}

void PosixChildProcess::terminate() {
    signalGroup(SIGTERM);
}

void PosixChildProcess::kill() {
    signalGroup(SIGKILL);
}

PosixChildProcessLauncher::PosixChildProcessLauncher() {
    // Writing to a pipe whose reader exited must fail with EPIPE instead of killing the host.
    ::signal(SIGPIPE, SIG_IGN);
}

std::vector<std::string> PosixChildProcessLauncher::environmentOf(const ProcessRequest& request) const {
    std::map<std::string, std::string> merged;
    if (request.inheritEnv) {
        for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
            const std::string text(*entry);
            const std::size_t eq = text.find('=');
            if (eq != std::string::npos) {
                merged[text.substr(0, eq)] = text.substr(eq + 1);
            }
        }
    }
    for (const auto& [key, value] : request.env) {
        merged[key] = value;
    }
    std::vector<std::string> lines;
    for (const auto& [key, value] : merged) {
        lines.push_back(key + "=" + value);
    }
    return lines;
}

Result<pid_t> PosixChildProcessLauncher::spawn(const ProcessRequest& request, int stdinRead, int stdoutWrite,
                                               int stderrWrite) const {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdinRead, 0);
    posix_spawn_file_actions_adddup2(&actions, stdoutWrite, 1);
    posix_spawn_file_actions_adddup2(&actions, stderrWrite, 2);
    if (request.cwd.has_value()) {
        posix_spawn_file_actions_addchdir_np(&actions, request.cwd->c_str());
    }
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF);
    posix_spawnattr_setpgroup(&attributes, 0);
    sigset_t defaults;
    sigfillset(&defaults);
    posix_spawnattr_setsigdefault(&attributes, &defaults);

    std::vector<std::string> argvStorage{request.command};
    argvStorage.insert(argvStorage.end(), request.args.begin(), request.args.end());
    std::vector<char*> argv;
    for (std::string& arg : argvStorage) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    std::vector<std::string> envStorage = environmentOf(request);
    std::vector<char*> envp;
    for (std::string& line : envStorage) {
        envp.push_back(line.data());
    }
    envp.push_back(nullptr);

    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, request.command.c_str(), &actions, &attributes, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (rc != 0) {
        return std::unexpected(Error{"spawn_failed", request.command + ": " + std::strerror(rc)});
    }
    return pid;
}

Result<std::unique_ptr<IChildProcess>> PosixChildProcessLauncher::launch(const ProcessRequest& request) {
    int stdinPipe[2];
    int stdoutPipe[2];
    int stderrPipe[2];
    if (::pipe2(stdinPipe, O_CLOEXEC) != 0 || ::pipe2(stdoutPipe, O_CLOEXEC) != 0 ||
        ::pipe2(stderrPipe, O_CLOEXEC) != 0) {
        return std::unexpected(Error{"pipe_failed", std::strerror(errno)});
    }
    const auto pid = spawn(request, stdinPipe[0], stdoutPipe[1], stderrPipe[1]);
    ::close(stdinPipe[0]);
    ::close(stdoutPipe[1]);
    ::close(stderrPipe[1]);
    if (!pid) {
        ::close(stdinPipe[1]);
        ::close(stdoutPipe[0]);
        ::close(stderrPipe[0]);
        return std::unexpected(pid.error());
    }
    return std::unique_ptr<IChildProcess>(std::make_unique<PosixChildProcess>(*pid, stdinPipe[1], stdoutPipe[0], stderrPipe[0]));
}
