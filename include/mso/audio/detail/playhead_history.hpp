#pragma once
/// @file
/// Latency compensation of the playhead: what the listener hears lags what the callback consumed.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace mso::audio::detail {

/// Fixed-size history of (output frame counter, position) points, recorded by the callback after
/// each period. The audible position is the newest point at least the device latency old, so the
/// reported playhead follows the sound instead of leading it by the device buffer (10-40 ms).
/// Real-time safe: a fixed array, no allocation. Resolution is one callback period.
///
/// Written for this engine rather than taken from a library (a recorded exception to owner rule 1):
/// it is latency-compensation logic over a fixed array, and no vendored library offers a bounded,
/// random-access history that the real-time callback may write without allocating.
class PlayheadHistory {
public:
    /// One recorded moment.
    struct Point {
        std::uint64_t output_frame = 0;  ///< frames written to the device when this was recorded
        std::uint64_t epoch = 0;         ///< PCM epoch of the position (0 = nothing played yet)
        std::int64_t position = 0;       ///< folded source position at that moment
    };

    /// Points kept: enough for 250 ms of latency at the shortest (~2 ms) callback periods.
    static constexpr std::size_t kCapacity = 128;

    /// Appends a point; `point.output_frame` must not decrease between calls.
    void record(const Point& point) noexcept {
        points_[next_] = point;
        next_ = (next_ + 1) % kCapacity;
        count_ = count_ < kCapacity ? count_ + 1 : kCapacity;
    }

    /// Forgets every point (a new device: its output frames and latency start afresh). Until a new
    /// point is old enough, audible() reports `heard`, the position last heard on the previous
    /// device, instead of nothing.
    void restart(const std::optional<Point>& heard) noexcept {
        next_ = 0;
        count_ = 0;
        carried_over_ = heard;
    }

    /// The newest point recorded at least `latency_frames` before `now_output_frame`. When every
    /// remembered point is younger: the point carried over by restart(), or nullopt (playback just
    /// began: nothing is audible yet).
    [[nodiscard]] std::optional<Point> audible(std::uint64_t now_output_frame,
                                               std::uint64_t latency_frames) const noexcept {
        if (now_output_frame >= latency_frames) {
            const std::uint64_t heard_frame = now_output_frame - latency_frames;
            for (std::size_t age = 0; age < count_; ++age) {
                const Point& point = points_[(next_ + kCapacity - 1 - age) % kCapacity];
                if (point.output_frame <= heard_frame) {
                    return point;
                }
            }
        }
        return carried_over_;
    }

private:
    std::array<Point, kCapacity> points_{};  ///< ring of recorded points
    std::size_t next_ = 0;                   ///< slot the next record() writes
    std::size_t count_ = 0;                  ///< valid points
    std::optional<Point> carried_over_;      ///< last heard on the previous device (see restart())
};

}  // namespace mso::audio::detail
