#include "src/base/posix_process_runner/posix_process_runner.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

extern char** environ;

std::vector<std::string> PosixProcessRunner::buildEnvironment(const ProcessRequest& request) const {
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

Result<pid_t> PosixProcessRunner::spawn(const ProcessRequest& request, int stdinRead,
                                        int outputWrite) const {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdinRead, 0);
    posix_spawn_file_actions_adddup2(&actions, outputWrite, 1);
    posix_spawn_file_actions_adddup2(&actions, outputWrite, 2);
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
    std::vector<std::string> envStorage = buildEnvironment(request);
    std::vector<char*> envp;
    for (std::string& line : envStorage) {
        envp.push_back(line.data());
    }
    envp.push_back(nullptr);

    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, request.command.c_str(), &actions, &attributes, argv.data(),
                                envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (rc != 0) {
        return std::unexpected(
            Error{"spawn_failed", request.command + ": " + std::strerror(rc)});
    }
    return pid;
}

void PosixProcessRunner::terminate(pid_t pid, int signal) const {
    if (kill(-pid, signal) != 0) {
        kill(pid, signal);
    }
}

void PosixProcessRunner::decodeStatus(int status, ProcessResult& result) const {
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = -1;
        result.termSignal = WTERMSIG(status);
    }
}

void PosixProcessRunner::pump(const ProcessRequest& request, pid_t pid, int stdinWrite,
                              int outputRead, int wakeRead, ProcessResult& result) const {
    using Clock = std::chrono::steady_clock;
    const bool hasTimeout = request.timeout.count() > 0;
    const Clock::time_point deadline = Clock::now() + request.timeout;
    Clock::time_point killAt = Clock::time_point::max();
    bool termSent = false;
    bool killSent = false;
    std::size_t stdinOffset = 0;
    char buffer[8192];
    while (true) {
        std::vector<pollfd> fds{{outputRead, POLLIN, 0}, {wakeRead, POLLIN, 0}};
        if (stdinWrite >= 0) {
            fds.push_back({stdinWrite, POLLOUT, 0});
        }
        long waitMs = -1;
        const auto now = Clock::now();
        if (!termSent && hasTimeout) {
            waitMs = std::max<long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        } else if (termSent && !killSent) {
            waitMs = std::max<long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(killAt - now).count());
        }
        const int ready = poll(fds.data(), fds.size(), static_cast<int>(waitMs));
        if (ready < 0 && errno != EINTR) {
            break;
        }
        const auto after = Clock::now();
        if (!termSent && hasTimeout && after >= deadline) {
            result.timedOut = true;
            terminate(pid, SIGTERM);
            termSent = true;
            killAt = after + std::chrono::milliseconds(kKillGraceMs);
        }
        if (termSent && !killSent && after >= killAt) {
            terminate(pid, SIGKILL);
            killSent = true;
        }
        if (ready > 0 && (fds[1].revents & POLLIN) != 0) {
            char drain[16];
            const ssize_t unused = read(wakeRead, drain, sizeof(drain));
            (void)unused;
            if (!termSent) {
                result.aborted = true;
                terminate(pid, SIGTERM);
                termSent = true;
                killAt = after + std::chrono::milliseconds(kKillGraceMs);
            }
        }
        if (ready > 0 && fds.size() > 2 && (fds[2].revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
            const std::string& data = request.stdinData;
            const ssize_t written = write(stdinWrite, data.data() + stdinOffset, data.size() - stdinOffset);
            if (written > 0) {
                stdinOffset += static_cast<std::size_t>(written);
            }
            if (written < 0 || stdinOffset >= data.size()) {
                close(stdinWrite);
                stdinWrite = -1;
            }
        }
        if (ready > 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            const ssize_t count = read(outputRead, buffer, sizeof(buffer));
            if (count <= 0 && errno != EINTR && errno != EAGAIN) {
                break;
            }
            if (count > 0) {
                const std::string_view chunk(buffer, static_cast<std::size_t>(count));
                if (request.captureOutput) {
                    result.output.append(chunk);
                }
                if (request.onOutput) {
                    request.onOutput(chunk);
                }
            }
        }
    }
    if (stdinWrite >= 0) {
        close(stdinWrite);
    }
}

Result<ProcessResult> PosixProcessRunner::run(const ProcessRequest& request) {
    int stdinPipe[2];
    int outPipe[2];
    int wakePipe[2];
    if (pipe2(stdinPipe, O_CLOEXEC) != 0 || pipe2(outPipe, O_CLOEXEC) != 0 ||
        pipe2(wakePipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        return std::unexpected(Error{"pipe_failed", std::strerror(errno)});
    }
    auto spawned = spawn(request, stdinPipe[0], outPipe[1]);
    close(stdinPipe[0]);
    close(outPipe[1]);
    if (!spawned.has_value()) {
        close(stdinPipe[1]);
        close(outPipe[0]);
        close(wakePipe[0]);
        close(wakePipe[1]);
        return std::unexpected(spawned.error());
    }
    const pid_t pid = *spawned;
    int stdinWrite = stdinPipe[1];
    if (request.stdinData.empty()) {
        close(stdinWrite);
        stdinWrite = -1;
    } else {
        fcntl(stdinWrite, F_SETFL, O_NONBLOCK);
        signal(SIGPIPE, SIG_IGN);
    }
    std::uint64_t listenerId = 0;
    const int wakeWrite = wakePipe[1];
    if (request.signal != nullptr) {
        listenerId = request.signal->onAbort([wakeWrite] {
            const char byte = 1;
            const ssize_t unused = write(wakeWrite, &byte, 1);
            (void)unused;
        });
    }
    ProcessResult result;
    pump(request, pid, stdinWrite, outPipe[0], wakePipe[0], result);
    if (request.signal != nullptr) {
        request.signal->removeListener(listenerId);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    decodeStatus(status, result);
    close(outPipe[0]);
    close(wakePipe[0]);
    close(wakePipe[1]);
    return result;
}
