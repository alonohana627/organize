#include "mso/audio/detail/output_watchdog.hpp"

namespace mso::audio::detail {

void OutputWatchdog::watch(bool follows_system_default, const OutputDeviceHealth& health,
                           Clock::time_point now) noexcept {
    watching_ = true;
    follows_system_default_ = follows_system_default;
    callback_count_ = health.callback_count;
    progress_at_ = now;
    stopped_at_.reset();
}

bool OutputWatchdog::lost(const OutputDeviceHealth& health, Clock::time_point now) noexcept {
    if (!watching_) {
        return false;
    }
    if (health.running) {
        if (stopped_at_ || health.callback_count != callback_count_) {
            // Advancing, or just back from a stop (a reroute): a fresh stall timeout from here.
            stopped_at_.reset();
            callback_count_ = health.callback_count;
            progress_at_ = now;
            return false;
        }
        return now - progress_at_ >= settings_.stall_timeout;
    }
    if (!follows_system_default_) {
        return true;  // only the backend stops a chosen endpoint, and nothing starts it again
    }
    if (!stopped_at_) {
        stopped_at_ = now;
    }
    return now - *stopped_at_ >= settings_.reroute_grace;
}

std::optional<OutputWatchdog::Clock::time_point> OutputWatchdog::next_check(Clock::time_point now) const noexcept {
    if (!watching_) {
        return std::nullopt;
    }
    return now + settings_.check_interval;
}

}  // namespace mso::audio::detail
