#include "mso/audio/detail/wakeup_signal.hpp"

#include <algorithm>
#include <cstdint>

#include <readerwriterqueue/atomicops.h>

namespace mso::audio::detail {

/// The vendored semaphore, kept out of the header (mso::readerwriterqueue is a PRIVATE dependency).
struct WakeupSignal::Semaphore {
    moodycamel::spsc_sema::LightweightSemaphore semaphore;  ///< one count per notify()
};

WakeupSignal::WakeupSignal() : semaphore_(std::make_unique<Semaphore>()) {}

WakeupSignal::~WakeupSignal() = default;

void WakeupSignal::notify() noexcept {
    semaphore_->semaphore.signal();
}

bool WakeupSignal::wait_for(std::chrono::microseconds timeout) noexcept {
    // The semaphore's kernel wait counts whole milliseconds on Windows and drops the rest, so a
    // remaining 0.4 ms would return at once and the caller would spin until its deadline. Rounding
    // up to the next millisecond sleeps through it instead (at most 1 ms late).
    constexpr std::int64_t kMicrosecondsPerMillisecond = 1000;
    const std::int64_t requested = std::max<std::int64_t>(timeout.count(), 0);
    const std::int64_t timeout_microseconds =
        (requested + kMicrosecondsPerMillisecond - 1) / kMicrosecondsPerMillisecond * kMicrosecondsPerMillisecond;
    if (!semaphore_->semaphore.wait(timeout_microseconds)) {
        return false;
    }
    // Notifications that piled up while this thread was busy are answered by this one wake-up.
    while (semaphore_->semaphore.tryWait()) {
    }
    return true;
}

}  // namespace mso::audio::detail
