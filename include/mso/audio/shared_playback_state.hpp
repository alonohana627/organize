#pragma once
/// @file
/// Atomics through which the control thread, the decoder thread and the audio callback communicate.
#include <atomic>
#include <cstdint>
#include <optional>

#include "mso/audio/fixed_text.hpp"
#include "mso/audio/preview_snapshot.hpp"
#include "mso/audio/transport.hpp"
#include "mso/domain/error.hpp"

namespace mso::audio {

/// Encodes an optional error code in one byte for lock-free publication.
class AtomicErrorCode {
public:
    void store(std::optional<ErrorCode> code) noexcept {
        value_.store(code ? static_cast<std::uint8_t>(*code) : kNone, std::memory_order_release);
    }
    std::optional<ErrorCode> load() const noexcept {
        const std::uint8_t value = value_.load(std::memory_order_acquire);
        if (value == kNone) {
            return std::nullopt;
        }
        return static_cast<ErrorCode>(value);
    }

private:
    static constexpr std::uint8_t kNone = 0xFF;
    std::atomic<std::uint8_t> value_{kNone};
};

/// Shared playback status. Every field has exactly one writing thread, named in its group, so no
/// read-modify-write races exist; readers tolerate fields from slightly different moments.
struct SharedPlaybackState {
    // ---- written by the control thread (open, device selection)

    /// Generation of the newest open() request; ahead of active_generation while loading.
    std::atomic<std::uint64_t> requested_generation{0};
    std::atomic<bool> output_available{false};
    std::atomic<std::uint32_t> output_sample_rate{0};
    std::atomic<std::uint32_t> output_period_frames{0};
    std::atomic<std::uint32_t> output_period_microseconds{0};
    AtomicText<kOutputDeviceNameCapacity> output_device_name;
    /// Bumped after every device (re)open; tells the decoder thread to adopt the new output rate.
    std::atomic<std::uint64_t> output_configuration{0};

    // ---- written by the decoder thread

    std::atomic<PreviewState> state{PreviewState::Empty};
    std::atomic<std::uint64_t> active_generation{0};
    AtomicErrorCode last_error;
    std::atomic<std::int64_t> total_frames{-1};  ///< -1 when unknown
    std::atomic<bool> total_frames_exact{false};
    std::atomic<int> source_sample_rate{0};
    std::atomic<int> source_channels{0};
    /// Current PCM epoch. Bumped by every open, seek, stop and re-anchor; ring blocks of older
    /// epochs are dropped unheard. Starts at 0 ("no audio yet"); real epochs start at 1.
    std::atomic<std::uint64_t> pcm_epoch{0};
    /// Source frame where the current epoch starts (published before pcm_epoch).
    std::atomic<std::int64_t> epoch_start_frame{0};
    /// Frames written to the ring for the current epoch (reset before each epoch bump).
    std::atomic<std::int64_t> epoch_produced_frames{0};
    /// Whether the callback should consume and play (it fades in/out on every change).
    std::atomic<bool> output_enabled{false};
    /// Volume target as linear gain 0..1; the callback smooths toward it per sample.
    std::atomic<float> target_volume{1.0f};
    /// Speed/pitch settings in effect, for snapshots.
    std::atomic<PlaybackMode> playback_mode{PlaybackMode::Varispeed};
    std::atomic<double> speed_ratio{1.0};
    std::atomic<double> pitch_semitones{0.0};

    // ---- written by the audio callback (or render_offline)

    /// Epoch of the last frame the callback played; position fields below belong to it.
    std::atomic<std::uint64_t> played_epoch{0};
    /// Audible source position (folded into the loop) after the last played frame.
    std::atomic<std::int64_t> played_frame{0};
    /// Frames played from played_epoch so far.
    std::atomic<std::int64_t> played_epoch_frames{0};
    /// Epoch whose end-of-stream block the callback has finished playing.
    std::atomic<std::uint64_t> ended_epoch{0};
    std::atomic<std::uint64_t> underrun_count{0};

    // ---- set by the device notification thread, cleared by the control thread

    /// The endpoint stopped unexpectedly. Sticky until the control thread opens another device
    /// (only then, with no device left to notify, is it cleared), so writers never overlap.
    std::atomic<bool> device_lost{false};
};

}  // namespace mso::audio
