#pragma once
/// @file
/// Lightweight wake-up for the decoder thread, usable from any non-real-time thread.
#include <atomic>
#include <chrono>
#include <semaphore>

namespace mso::audio::detail {

/// Wakes a worker sleeping in wait_for(). Notifications that arrive before the worker wakes collapse
/// into one, which also keeps the binary semaphore's count within its maximum. A notification racing
/// with a wake already in progress may be absorbed by it; the worker re-checks its inputs after every
/// wake and never sleeps longer than its timeout, so such a race only costs one bounded poll.
class WakeupSignal {
public:
    /// Requests a wake-up. Not for the real-time callback (it may enter the kernel).
    void notify() noexcept {
        if (!pending_.exchange(true, std::memory_order_acq_rel)) {
            semaphore_.release();
        }
    }

    /// Sleeps until notified or until `timeout` elapses. Returns true when woken by notify().
    template <class Rep, class Period>
    bool wait_for(std::chrono::duration<Rep, Period> timeout) {
        const bool notified = semaphore_.try_acquire_for(timeout);
        if (notified) {
            pending_.store(false, std::memory_order_release);
        }
        return notified;
    }

private:
    std::binary_semaphore semaphore_{0};  ///< released once per collapsed batch of notifications
    std::atomic<bool> pending_{false};    ///< a release is outstanding; further notify() calls are no-ops
};

}  // namespace mso::audio::detail
