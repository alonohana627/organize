#pragma once
/// @file
/// Atomics through which the control thread, the decoder thread and the audio callback communicate.
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>

#include "mso/audio/detail/cache_line.hpp"
#include "mso/audio/detail/seqlock.hpp"
#include "mso/audio/fixed_text.hpp"
#include "mso/audio/preview_snapshot.hpp"
#include "mso/audio/transport.hpp"
#include "mso/domain/error.hpp"

namespace mso::audio::detail {

/// Encodes an optional error code in one byte for lock-free publication.
class AtomicErrorCode {
public:
    /// Publishes `code` (nullopt clears it).
    void store(std::optional<ErrorCode> code) noexcept {
        value_.store(code ? static_cast<std::uint8_t>(*code) : kNone, std::memory_order_release);
    }
    /// The most recently published code.
    [[nodiscard]] std::optional<ErrorCode> load() const noexcept {
        const std::uint8_t value = value_.load(std::memory_order_acquire);
        if (value == kNone) {
            return std::nullopt;
        }
        return static_cast<ErrorCode>(value);
    }

private:
    static constexpr std::uint8_t kNone = 0xFF;  ///< "no error"; never a valid ErrorCode
    std::atomic<std::uint8_t> value_{kNone};     ///< encoded code
};

/// The PCM epoch together with "output enabled", packed into one 64-bit word so the callback always
/// sees a matching pair: Stop publishes "new epoch, disabled" in a single store, and a callback can
/// never play the new epoch's audio under the old "enabled" flag.
struct TransportGate {
    /// Current PCM epoch. Bumped by every open, seek, stop and re-anchor; ring blocks of older
    /// epochs are dropped unheard. 0 means "no audio yet"; real epochs start at 1.
    std::uint64_t epoch = 0;
    /// Whether the callback should consume and play.
    bool output_enabled = false;

    /// Single-word form for the atomic.
    [[nodiscard]] std::uint64_t packed() const noexcept { return (epoch << 1) | (output_enabled ? 1u : 0u); }
    /// Inverse of packed().
    [[nodiscard]] static TransportGate unpacked(std::uint64_t word) noexcept { return {word >> 1, (word & 1u) != 0}; }
};

/// What the callback has played, published once per callback through a Seqlock so readers always
/// get fields from the same moment.
struct PlaybackProgress {
    /// Epoch of the last frame consumed from the ring; the consumption fields belong to it.
    std::uint64_t consumed_epoch = 0;
    /// Frames consumed from consumed_epoch so far (the decoder thread's buffer accounting).
    std::int64_t consumed_frames = 0;
    /// Folded source position after the last consumed frame: where the ring's audio continues. Shape
    /// changes (speed, pitch, mode, loop, seam fades) re-anchor here, so the audio already in the
    /// device buffer is followed seamlessly instead of being played twice.
    std::int64_t consumed_position = 0;
    /// Epoch of the frame the listener hears now (consumption delayed by the output latency).
    std::uint64_t audible_epoch = 0;
    /// Folded source position the listener hears now.
    std::int64_t audible_frame = 0;
};

/// Frames the decoder thread has written to the ring for one epoch, published as a pair so a
/// reader never matches one epoch's count with another epoch's consumption.
struct EpochProduction {
    std::uint64_t epoch = 0;  ///< epoch the count belongs to
    std::int64_t frames = 0;  ///< frames written for that epoch so far
};

/// Speed, pitch, mode and loop settings in effect, published by the decoder thread for snapshots.
struct PlaybackSettings {
    PlaybackMode mode = PlaybackMode::Varispeed;  ///< speed/pitch relationship
    bool seam_fades = false;                      ///< loop-seam fades on
    double speed_ratio = 1.0;                     ///< speed in effect
    double pitch_semitones = 0.0;                 ///< pitch setting
    std::int64_t loop_start_frame = 0;            ///< effective loop start
    std::int64_t loop_end_frame = 0;              ///< effective loop end; loop inactive when <= start
};

/// Device facts published by the output thread when an endpoint opens.
struct OutputFacts {
    std::uint32_t sample_rate = 0;              ///< stream rate
    std::uint32_t period_frames = 0;            ///< actual endpoint period
    std::uint32_t period_microseconds = 0;      ///< the same period in microseconds
    std::uint32_t latency_frames = 0;           ///< device buffer, in stream frames
    std::uint32_t latency_microseconds = 0;     ///< the same latency in microseconds
    FixedText<kOutputDeviceNameCapacity> name;  ///< endpoint display name
};

/// Shared playback status. Fields are grouped by their single writing thread, each group on its own
/// cache lines, so the callback's per-period stores never contend with the other threads' lines.
/// Readers tolerate fields from slightly different moments except where a Seqlock or the packed
/// TransportGate keeps related fields together.
struct SharedPlaybackState {
    // ---- written by the control thread (open) and the output thread (device events)

    /// Generation of the newest open() request; ahead of active_generation while loading. Published
    /// by PlaybackController::request_source together with the request itself.
    alignas(kCacheLineBytes) std::atomic<std::uint64_t> requested_generation{0};
    /// An endpoint is open (Offline counts as one).
    std::atomic<bool> output_available{false};
    /// Facts about the open endpoint.
    Seqlock<OutputFacts> output_facts;
    /// Bumped after every device (re)open, after output_facts; tells the decoder thread to adopt
    /// the new output rate.
    std::atomic<std::uint64_t> output_configuration{0};

    // ---- written by the decoder thread

    /// Epoch and output switch, packed (see TransportGate).
    alignas(kCacheLineBytes) std::atomic<std::uint64_t> transport_gate{0};
    /// Source frame where the current epoch starts (published before transport_gate).
    std::atomic<std::int64_t> epoch_start_frame{0};
    /// Frames written to the ring for the current epoch, with that epoch (reset at each epoch bump).
    Seqlock<EpochProduction> production;
    /// Volume target as linear gain 0..1; the callback smooths toward it per sample.
    std::atomic<float> target_volume{1.0f};
    /// Transport state reported to the UI.
    std::atomic<PreviewState> state{PreviewState::Empty};
    /// Generation whose source the other decoder-thread fields describe.
    std::atomic<std::uint64_t> active_generation{0};
    /// Latest failure (see PreviewSnapshot::last_error).
    AtomicErrorCode last_error;
    /// Source length; -1 when unknown.
    std::atomic<std::int64_t> total_frames{-1};
    /// Whether total_frames is exact.
    std::atomic<bool> total_frames_exact{false};
    /// Source rate in frames per second.
    std::atomic<int> source_sample_rate{0};
    /// Source channel count.
    std::atomic<int> source_channels{0};
    /// Speed/pitch/mode/loop in effect.
    Seqlock<PlaybackSettings> settings;

    // ---- written by the audio callback (or render_offline)

    /// Consumption and audible position (see PlaybackProgress).
    alignas(kCacheLineBytes) Seqlock<PlaybackProgress> progress;
    /// Epoch whose end-of-stream block the callback has finished playing.
    std::atomic<std::uint64_t> ended_epoch{0};
    /// Callbacks that starved while playing.
    std::atomic<std::uint64_t> underrun_count{0};

    // ---- set by the device notification thread, cleared by the output thread

    /// The endpoint stopped unexpectedly, or none could be opened. Sticky until the output thread
    /// opens another device (only then, with no device left to notify, is it cleared), so writers
    /// never overlap. While it is set the output thread re-opens an endpoint with backoff.
    alignas(kCacheLineBytes) std::atomic<bool> device_lost{false};

    // ---- set by the output thread, taken by the decoder thread

    /// restart_output_device() asked for the transport to come back exactly as it was (Playing
    /// included). The decoder thread takes it with the next device configuration; without it a
    /// source that was playing when the endpoint was lost comes back Paused.
    std::atomic<bool> resume_on_restore{false};
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::int64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<PreviewState>::is_always_lock_free);

}  // namespace mso::audio::detail
