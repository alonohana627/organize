#pragma once
/// @file
/// Decides from what the output thread can observe whether an open endpoint is lost.
#include <chrono>
#include <cstdint>
#include <optional>

#include "mso/audio/detail/output_device.hpp"

namespace mso::audio::detail {

/// How patient the OutputWatchdog is.
struct OutputWatchdogSettings {
    /// Time between two looks at a watched endpoint. Backend events wake the output thread for an
    /// extra look at once.
    std::chrono::milliseconds check_interval{250};
    /// A running endpoint whose data callback has not run for this long is lost (a stuck driver,
    /// or an unplugged endpoint whose backend stop failed and therefore announced nothing).
    std::chrono::milliseconds stall_timeout{1000};
    /// A system-default endpoint that the backend stopped may be on its way to the new default
    /// device (miniaudio stops, reroutes and restarts it). It is lost only when it stays stopped
    /// this long.
    std::chrono::milliseconds reroute_grace{3000};
};

/// The output thread's verdict on one endpoint, from its health alone (OutputDeviceHealth):
///  - running with an advancing callback: healthy;
///  - running with a callback that has not advanced for `stall_timeout`: lost;
///  - stopped by the backend: lost at once for an explicitly chosen endpoint (nothing restarts
///    it), and after `reroute_grace` for a system-default endpoint (the backend restarts it
///    itself after a default-device change).
///
/// Backend notifications only make the output thread look sooner; they never decide on their own,
/// because "stopped" also announces a reroute and a failed stop announces nothing.
/// Not thread-safe: the output thread owns it.
class OutputWatchdog {
public:
    using Clock = std::chrono::steady_clock;

    explicit OutputWatchdog(const OutputWatchdogSettings& settings) noexcept : settings_(settings) {}

    /// Starts watching an endpoint that was just started (or found working again): its callback
    /// has `health.callback_count` runs so far, and it is given a full stall timeout from `now`.
    void watch(bool follows_system_default, const OutputDeviceHealth& health, Clock::time_point now) noexcept;

    /// Stops watching: no endpoint is open, or the open one was stopped on purpose or found lost.
    void unwatch() noexcept { watching_ = false; }

    /// True while an endpoint is watched.
    [[nodiscard]] bool watching() const noexcept { return watching_; }

    /// Looks at the watched endpoint once; true when it is lost. Always false while not watching.
    [[nodiscard]] bool lost(const OutputDeviceHealth& health, Clock::time_point now) noexcept;

    /// When the next regular look is due after one at `now`; nullopt while not watching.
    [[nodiscard]] std::optional<Clock::time_point> next_check(Clock::time_point now) const noexcept;

private:
    const OutputWatchdogSettings settings_;        ///< patience
    bool watching_ = false;                        ///< an endpoint is watched
    bool follows_system_default_ = false;          ///< the backend may restart it by itself
    std::uint64_t callback_count_ = 0;             ///< callback count at the last progress
    Clock::time_point progress_at_{};              ///< when the callback last advanced (or watching began)
    std::optional<Clock::time_point> stopped_at_;  ///< first look that found it stopped, while it stays so
};

}  // namespace mso::audio::detail
