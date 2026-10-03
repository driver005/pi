export module pi.platform_services;

import std;
import pi.base.base64_codec;
import pi.child_process_launcher;
import pi.base.boring_crypto;
import pi.base.curl_http_client;
import pi.base.posix_dynamic_libraries;
import pi.base.posix_file_lock;
import pi.base.posix_file_system;
import pi.base.posix_process_runner;
import pi.base.stderr_logger;
import pi.base.system_clock;
import pi.base.system_environment;
import pi.base.thread_pool;
import pi.base.thread_sleeper;
import pi.base.uuid7_generator;

/** The operating-system backed services every part of the application shares. */
export class PlatformServices {
public:
    explicit PlatformServices(std::size_t workers = 8)
        : m_ids(m_clock),
          m_executor(workers),
          m_logger(m_clock, LogLevel::Info) {}

    SystemClock& clock() {
        return m_clock;
    }

    Uuid7Generator& ids() {
        return m_ids;
    }

    SystemEnvironment& environment() {
        return m_environment;
    }

    PosixFileSystem& files() {
        return m_files;
    }

    PosixFileLock& locks() {
        return m_locks;
    }

    PosixProcessRunner& processes() {
        return m_processes;
    }

    ChildProcessLauncher& children() {
        return m_children;
    }

    BoringCrypto& crypto() {
        return m_crypto;
    }

    Base64Codec& base64() {
        return m_base64;
    }

    ThreadSleeper& sleeper() {
        return m_sleeper;
    }

    CurlHttpClient& http() {
        return m_http;
    }

    ThreadPool& executor() {
        return m_executor;
    }

    PosixDynamicLibraries& libraries() {
        return m_libraries;
    }

    StderrLogger& logger() {
        return m_logger;
    }

private:
    SystemClock m_clock;
    Uuid7Generator m_ids;
    SystemEnvironment m_environment;
    PosixFileSystem m_files;
    PosixFileLock m_locks;
    PosixProcessRunner m_processes;
    ChildProcessLauncher m_children;
    BoringCrypto m_crypto;
    Base64Codec m_base64;
    ThreadSleeper m_sleeper;
    CurlHttpClient m_http;
    ThreadPool m_executor;
    PosixDynamicLibraries m_libraries;
    StderrLogger m_logger;
};
