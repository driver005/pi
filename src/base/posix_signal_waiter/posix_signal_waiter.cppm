module;

#include <csignal>
#include <pthread.h>

export module pi.base.posix_signal_waiter;

import std;

/**
 * Waits for the signals that end a server (SIGINT, SIGTERM, SIGHUP). block() must run on the main
 * thread before any other thread starts, so every thread inherits the mask and the signals are only
 * ever received by wait().
 */
export class PosixSignalWaiter {
public:
    PosixSignalWaiter() {
        sigemptyset(&m_signals);
        sigaddset(&m_signals, SIGINT);
        sigaddset(&m_signals, SIGTERM);
        sigaddset(&m_signals, SIGHUP);
    }

    void block() {
        pthread_sigmask(SIG_BLOCK, &m_signals, nullptr);
    }

    /** Blocks until one of the signals arrives; returns its number. */
    int wait() {
        int received = 0;
        while (sigwait(&m_signals, &received) != 0) {
        }
        return received;
    }

private:
    sigset_t m_signals;
};
