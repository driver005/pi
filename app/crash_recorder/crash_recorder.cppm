module;

#include <csignal>
#include <cstdlib>
#include <execinfo.h>
#include <string.h>
#include <unistd.h>

export module pi.crash_recorder;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_file_system;
import pi.support.crash_log;

/**
 * Records a crash of the process in `<agent dir>/crashes.json`: an uncaught C++ exception or std::terminate (kind
 * `uncaught_exception`) and the fatal signals SIGSEGV, SIGBUS, SIGFPE, SIGILL and SIGABRT (kind `fatal_error`), with the
 * backtrace. It works on a process that is already failing, best effort and not async-signal-safe, then lets the default
 * action run so the exit status and core dump stay as they were. One recorder is active per process.
 */
export class CrashRecorder {
public:
    CrashRecorder(IFileSystem& files, const IClock& clock, std::string agentDir, std::string cwd)
        : m_log(files, clock),
          m_agentDir(std::move(agentDir)),
          m_cwd(std::move(cwd)) {}

    ~CrashRecorder() {
        uninstall();
    }

    CrashRecorder(const CrashRecorder&) = delete;
    CrashRecorder& operator=(const CrashRecorder&) = delete;

    /** The session file to name in crash records; updated as sessions change. */
    void setSessionFile(const std::optional<std::string>& file) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_sessionFile = file;
    }

    void install() {
        m_activeRecorder = this;
        m_previousTerminate = std::set_terminate([] {
            if (m_activeRecorder != nullptr) {
                m_activeRecorder->record("uncaught_exception", "std::terminate called");
            }
            std::abort();
        });
        for (const int signal : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
            struct sigaction action {};
            action.sa_handler = [](int number) {
                if (m_activeRecorder != nullptr) {
                    CrashRecorder* recorder = m_activeRecorder;
                    m_activeRecorder = nullptr;
                    recorder->record("fatal_error", std::string("Fatal signal ") + strsignal(number));
                }
                std::signal(number, SIG_DFL);
                ::raise(number);
            };
            sigemptyset(&action.sa_mask);
            sigaction(signal, &action, nullptr);
        }
    }

    void uninstall() {
        if (m_activeRecorder != this) {
            return;
        }
        m_activeRecorder = nullptr;
        std::set_terminate(m_previousTerminate);
        for (const int signal : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
            std::signal(signal, SIG_DFL);
        }
    }

    /** Appends a crash record now (what the handlers do); true when it was written. */
    bool record(const std::string& kind, const std::string& message) {
        std::array<void*, 64> frames{};
        const int count = backtrace(frames.data(), static_cast<int>(frames.size()));
        std::string stack = message;
        if (char** symbols = backtrace_symbols(frames.data(), count)) {
            for (int i = 0; i < count; ++i) {
                stack += "\n    at " + std::string(symbols[i]);
            }
            std::free(symbols);
        }
        std::optional<std::string> sessionFile;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            sessionFile = m_sessionFile;
        }
        return m_log.record(m_log.path(m_agentDir), kind, message, stack, sessionFile, m_cwd).has_value();
    }

    /** The newest recent crash nobody was told about yet, as a line for the user; empty when there is none. */
    std::string notice() {
        const auto crash = m_log.takeUnnotified(m_log.path(m_agentDir));
        if (!crash) {
            return "";
        }
        return "pi crashed last time (" + crash->kind + ": " + crash->message + " at " + crash->timestamp + "). Send a bug report with the bug_report command to include its details.";
    }

private:
    inline static CrashRecorder* m_activeRecorder = nullptr;

    CrashLog m_log;
    std::string m_agentDir;
    std::string m_cwd;
    std::mutex m_mutex;
    std::optional<std::string> m_sessionFile;
    std::terminate_handler m_previousTerminate = nullptr;
};
