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
    const std::int64_t timeout_microseconds = std::max<std::int64_t>(timeout.count(), 0);
    if (!semaphore_->semaphore.wait(timeout_microseconds)) {
        return false;
    }
    // Notifications that piled up while this thread was busy are answered by this one wake-up.
    while (semaphore_->semaphore.tryWait()) {
    }
    return true;
}

}  // namespace mso::audio::detail
