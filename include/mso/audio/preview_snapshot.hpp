#pragma once
/// @file
/// PreviewSnapshot: the read-only view of the preview engine a UI polls (e.g. on a 30 Hz timer).
#include <cstddef>
#include <cstdint>
#include <optional>

#include "mso/audio/fixed_text.hpp"
#include "mso/audio/transport.hpp"
#include "mso/domain/error.hpp"

namespace mso::audio {

/// Room for miniaudio's longest endpoint name (MA_MAX_DEVICE_NAME_LENGTH).
inline constexpr std::size_t kOutputDeviceNameCapacity = 255;
using OutputDeviceName = FixedText<kOutputDeviceNameCapacity>;

/// Point-in-time transport and device status, assembled from atomics only (no decoder or device
/// calls), so taking one never blocks. Fields written by different threads may be a few
/// milliseconds apart; the next snapshot catches up.
struct PreviewSnapshot {
    PreviewState state = PreviewState::Empty;
    /// Increments with every open(); identifies which source the other source fields describe.
    std::uint64_t generation = 0;

    /// Audible position in source frames (what the listener hears now, folded into the loop region).
    std::int64_t position_frames = 0;
    /// Source length in frames when the decoder knows it.
    std::optional<std::int64_t> total_frames;
    /// False when total_frames is an estimate (VBR, bitrate-derived).
    bool total_frames_exact = false;
    int source_sample_rate = 0;
    int source_channels = 0;

    OutputDeviceName output_device_name;
    bool output_device_available = false;
    /// Rate of the stereo float stream handed to the device (the device's native rate by default).
    std::uint32_t output_sample_rate = 0;
    /// Actual period negotiated with the endpoint, in device frames and in microseconds.
    std::uint32_t output_period_frames = 0;
    std::uint32_t output_period_microseconds = 0;

    /// Preview volume target as linear gain 0..1 (the callback glides toward it).
    float volume = 1.0f;
    /// Active speed/pitch relationship and the settings in effect (V2).
    PlaybackMode playback_mode = PlaybackMode::Varispeed;
    double speed_ratio = 1.0;
    /// Pitch setting; audible only in IndependentTempoPitch mode.
    double pitch_semitones = 0.0;

    /// Decoded audio of the current source generation waiting in the PCM ring.
    std::uint32_t buffered_milliseconds = 0;
    /// Callbacks that found the ring empty while playing (each wrote silence instead).
    std::uint64_t underrun_count = 0;
    /// Transport commands rejected because the mailbox was full (Stop is never rejected).
    std::uint64_t dropped_command_count = 0;

    /// Most recent failure: FILE_MISSING / CORRUPT_AUDIO / UNSUPPORTED_CODEC from the source,
    /// DEVICE_LOST from the endpoint. Cleared by a successful open() or device selection.
    std::optional<ErrorCode> last_error;
};

}  // namespace mso::audio
