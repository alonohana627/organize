#pragma once
/// @file
/// Ctrl+C as a cooperative stop request for the duration of one command.
///
/// The signal handler (POSIX `sigaction`) or console control handler (Windows
/// `SetConsoleCtrlHandler`) only stores a lock-free atomic flag, which is async-signal-safe. A
/// watcher thread turns the flag into a std::stop_source request, so long work (import, analyze,
/// export, play) can observe an ordinary std::stop_token. The previous handler is restored when
/// the scope ends. Only one scope may be active at a time. A second Ctrl+C while the first is
/// still being handled is left to the default handling and ends the process (a hung stop can be
/// forced).
#include <stop_token>
#include <thread>

namespace mso::cli {

/// Turns Ctrl+C into a std::stop_token request while it exists (one active scope per process).
class InterruptScope {
public:
    /// Installs the handler and starts the watcher thread.
    InterruptScope();
    /// Stops the watcher and restores the previous handler.
    ~InterruptScope();
    InterruptScope(const InterruptScope&) = delete;
    InterruptScope& operator=(const InterruptScope&) = delete;

    /// Becomes stop-requested after Ctrl+C.
    std::stop_token token() const { return stop_.get_token(); }
    /// True after Ctrl+C.
    bool interrupted() const { return stop_.stop_requested(); }

    /// Behaves exactly like a first Ctrl+C delivered to this process (tests). Called while no
    /// scope is active, the interrupt is pending and the next scope starts already interrupted.
    static void simulate_interrupt() noexcept;

private:
    std::stop_source stop_;
    std::jthread watcher_;
};

}  // namespace mso::cli
