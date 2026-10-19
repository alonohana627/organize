#pragma once
/// @file
/// Wake-up of one waiting engine thread (the decoder thread, the output thread), usable from any
/// non-real-time thread.
#include <chrono>
#include <memory>

namespace mso::audio::detail {

/// Wakes the one thread sleeping in wait_for(). Built on the vendored moodycamel
/// LightweightSemaphore (the semaphore of moodycamel::BlockingReaderWriterQueue): notify() signals
/// it, and a woken waiter takes every further pending signal at once, so notifications that arrive
/// before the waiter wakes collapse into one wake-up. The waiter re-checks all of its inputs after
/// every wake and never sleeps longer than its timeout, so a notification absorbed by a wake already
/// in progress only costs one bounded poll.
class WakeupSignal {
public:
    /// Creates an unsignalled wake-up. Throws std::bad_alloc only.
    WakeupSignal();
    ~WakeupSignal();
    WakeupSignal(const WakeupSignal&) = delete;
    WakeupSignal& operator=(const WakeupSignal&) = delete;

    /// Requests a wake-up. Any number of threads may notify; not for the real-time callback (it may
    /// enter the kernel).
    void notify() noexcept;

    /// Sleeps until notified or until `timeout` elapses. Returns true when woken by notify().
    /// Only one thread may wait on a given signal.
    bool wait_for(std::chrono::microseconds timeout) noexcept;

private:
    struct Semaphore;
    std::unique_ptr<Semaphore> semaphore_;  ///< the moodycamel semaphore (kept out of this header)
};

}  // namespace mso::audio::detail
