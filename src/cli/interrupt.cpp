#include "mso/cli/interrupt.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

namespace mso::cli {

namespace {

/// Set by the handler; read by the watcher. Lock-free, hence safe to store from a signal handler.
std::atomic<bool> interrupt_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

/// How often the watcher looks at the flag (Ctrl+C latency; no busy loop).
constexpr auto kWatchInterval = std::chrono::milliseconds(50);

#ifdef _WIN32
/// Console control handler: the first Ctrl+C / Ctrl+Break is recorded and the process keeps
/// running to stop cooperatively; a second one is left to the default handler, which ends the
/// process, so a stop that hangs can still be forced.
BOOL WINAPI on_console_control(DWORD control) {
    if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT) return FALSE;
    if (interrupt_requested.exchange(true)) return FALSE;  // second press: default handling
    return TRUE;
}
#else
/// The SIGINT disposition to restore when the scope ends.
struct sigaction previous_action{};

/// SIGINT handler: only stores the lock-free flag (async-signal-safe). Installed with
/// SA_RESETHAND, so a second SIGINT gets the default disposition and ends the process.
void on_interrupt_signal(int) {
    interrupt_requested.store(true);  // the only thing done in signal context
}
#endif

}  // namespace

InterruptScope::InterruptScope() {
    // The flag is cleared when a scope ends, so one still set here is an interrupt that arrived
    // while no scope was active (only simulate_interrupt() can do that): honour it at once.
    if (interrupt_requested.load()) stop_.request_stop();
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_control, TRUE);
#else
    struct sigaction action{};
    action.sa_handler = on_interrupt_signal;
    action.sa_flags = SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, &previous_action);
#endif
    watcher_ = std::jthread([this](std::stop_token stop) {
        // Exception barrier: if waiting ever fails, treat it like Ctrl+C so the command stops
        // cooperatively instead of the process terminating.
        try {
            std::mutex sleep_mutex;
            std::condition_variable_any sleeper;
            while (!stop.stop_requested()) {
                if (interrupt_requested.load()) {
                    stop_.request_stop();
                    return;
                }
                std::unique_lock lock(sleep_mutex);
                sleeper.wait_for(lock, stop, kWatchInterval, [] { return false; });
            }
        } catch (const std::exception&) {
            stop_.request_stop();
        }
    });
}

InterruptScope::~InterruptScope() {
    watcher_ = {};  // request stop and join before the handler goes away
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_control, FALSE);
#else
    sigaction(SIGINT, &previous_action, nullptr);
#endif
    interrupt_requested.store(false);
}

void InterruptScope::simulate_interrupt() noexcept {
    interrupt_requested.store(true);
}

}  // namespace mso::cli
