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
    explicit PlatformServices(std::size_t workers = 8);

    SystemClock& clock();
    Uuid7Generator& ids();
    SystemEnvironment& environment();
    PosixFileSystem& files();
    PosixFileLock& locks();
    PosixProcessRunner& processes();
    ChildProcessLauncher& children();
    BoringCrypto& crypto();
    Base64Codec& base64();
    ThreadSleeper& sleeper();
    CurlHttpClient& http();
    ThreadPool& executor();
    PosixDynamicLibraries& libraries();
    StderrLogger& logger();

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

PlatformServices::PlatformServices(std::size_t workers)
    : m_ids(m_clock), m_executor(workers), m_logger(m_clock, LogLevel::Info) {}

SystemClock& PlatformServices::clock() {
    return m_clock;
}

Uuid7Generator& PlatformServices::ids() {
    return m_ids;
}

SystemEnvironment& PlatformServices::environment() {
    return m_environment;
}

PosixFileSystem& PlatformServices::files() {
    return m_files;
}

PosixFileLock& PlatformServices::locks() {
    return m_locks;
}

PosixProcessRunner& PlatformServices::processes() {
    return m_processes;
}

ChildProcessLauncher& PlatformServices::children() {
    return m_children;
}

BoringCrypto& PlatformServices::crypto() {
    return m_crypto;
}

Base64Codec& PlatformServices::base64() {
    return m_base64;
}

ThreadSleeper& PlatformServices::sleeper() {
    return m_sleeper;
}

CurlHttpClient& PlatformServices::http() {
    return m_http;
}

ThreadPool& PlatformServices::executor() {
    return m_executor;
}

PosixDynamicLibraries& PlatformServices::libraries() {
    return m_libraries;
}

StderrLogger& PlatformServices::logger() {
    return m_logger;
}
