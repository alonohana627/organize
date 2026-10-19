#pragma once
/// @file
/// Transport vocabulary of the preview engine: the commands a UI posts and the states it observes.
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace mso::audio {

/// Half-open range [start_frame, end_frame) of source frames that playback repeats.
struct LoopRegion {
    std::int64_t start_frame = 0;
    std::int64_t end_frame = 0;

    bool operator==(const LoopRegion&) const = default;
};

/// Speed limits (0.25–4×). How speed affects pitch depends on the PlaybackMode.
inline constexpr double kMinimumSpeedRatio = 0.25;
inline constexpr double kMaximumSpeedRatio = 4.0;
/// Pitch shift limit of the independent tempo/pitch mode, in semitones either way.
inline constexpr double kMaximumPitchSemitones = 24.0;

/// How speed and pitch relate during preview. (V2)
enum class PlaybackMode : std::uint8_t {
    /// Tape-style: speed changes the resampling ratio, so pitch follows speed; the pitch setting
    /// is kept but has no effect. Cheapest and artifact-free.
    Varispeed,
    /// Speed changes tempo only and the pitch setting transposes independently, via a phase-vocoder
    /// time stretch (signalsmith-stretch) on the decoder thread. Extreme settings may sound smeared.
    IndependentTempoPitch,
};

/// Commands accepted by PreviewEngine::post(). Each is a small value type; see TransportCommand.
namespace transport {

/// Starts or resumes playback from the current position. Ignored without a playable source.
struct Play {};
/// Holds the current position; Play resumes from it.
struct Pause {};
/// Pause when playing, otherwise Play (the Space-bar behavior).
struct TogglePlayPause {};
/// Stops and rewinds to the start. Always accepted, even when the mailbox is otherwise full.
struct Stop {};
/// Moves to an absolute source frame (clamped to the source). Repeated seeks coalesce.
struct SeekToFrame {
    std::int64_t source_frame = 0;
};
/// Moves to a fraction 0..1 of the source length (needs a known length). Repeated seeks coalesce.
struct SeekToFraction {
    double fraction = 0.0;
};
/// Preview volume in decibels, at most 0 dB; the callback smooths every change per sample.
struct SetVolumeDecibels {
    double decibels = 0.0;
};
/// Preview volume as linear gain 0..1; the callback smooths every change per sample.
struct SetVolumeLinear {
    double gain = 1.0;
};
/// Loops the given region, or turns looping off with std::nullopt. (V2)
struct SetLoop {
    std::optional<LoopRegion> region;
};
/// Speed ratio 0.25..4. In Varispeed mode pitch follows speed; in IndependentTempoPitch mode only
/// the tempo changes. (V2)
struct SetSpeed {
    double ratio = 1.0;
};
/// Pitch shift in semitones, -24..+24; heard in IndependentTempoPitch mode only. (V2)
struct SetPitch {
    double semitones = 0.0;
};
/// Chooses how speed and pitch interact. (V2)
struct SetPlaybackMode {
    PlaybackMode mode = PlaybackMode::Varispeed;
};

}  // namespace transport

/// Everything a UI can ask the transport to do, posted through PreviewEngine::post().
using TransportCommand =
    std::variant<transport::Play, transport::Pause, transport::TogglePlayPause, transport::Stop, transport::SeekToFrame,
                 transport::SeekToFraction, transport::SetVolumeDecibels, transport::SetVolumeLinear,
                 transport::SetLoop, transport::SetSpeed, transport::SetPitch, transport::SetPlaybackMode>;

/// Observable transport state. `Loading` covers the time between open() and the decoder being ready.
enum class PreviewState : std::uint8_t {
    Empty,    ///< nothing opened yet
    Loading,  ///< open() requested; the decoder thread is opening the source
    Ready,    ///< source opened and prebuffering; never played since opening
    Playing,
    Paused,
    Stopped,  ///< rewound to the start, either by Stop or by reaching the end of the source
    Error,    ///< see PreviewSnapshot::last_error; replay needs a new open() (or a device for DEVICE_LOST)
};

/// Stable lowercase name of a state, for diagnostics and logs.
std::string_view to_string(PreviewState state) noexcept;
/// Stable lowercase name of a playback mode ("varispeed", "independent_tempo_pitch").
std::string_view to_string(PlaybackMode mode) noexcept;

/// Converts decibels to linear gain clamped to 0..1: 0 dB is unity, -inf/NaN/below -120 dB is silence.
float linear_gain_from_decibels(double decibels) noexcept;

/// Clamps a linear gain to 0..1; NaN becomes silence.
float clamped_linear_gain(double gain) noexcept;

/// Clamps a pitch shift to ±kMaximumPitchSemitones; NaN becomes 0.
double clamped_pitch_semitones(double semitones) noexcept;

}  // namespace mso::audio
