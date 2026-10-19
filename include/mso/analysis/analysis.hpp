#pragma once
/// @file
/// Explicit audio analysis of one file (docs/05_AUDIO_INTELLIGENCE.md). Pure computation: no
/// catalog writes. Runs inside the mso-worker child process; results return to the supervisor,
/// which fences them by lease, cancel epoch and content revision before committing. The preview
/// waveform the desktop app draws for the selected file has its own narrower entry point in
/// waveform_preview.hpp.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "mso/analysis/limits.hpp"
#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"

namespace mso::analysis {

/// Independently selectable analysis stages. A profile enables a subset.
enum class Stage { Waveform, Loudness, Tempo, Key, ByteHash, PcmHash };

/// Every stage, in declaration order.
inline constexpr Stage kAllStages[] = {Stage::Waveform, Stage::Loudness, Stage::Tempo,
                                       Stage::Key,      Stage::ByteHash, Stage::PcmHash};

/// Stable wire and profile name: "waveform", "loudness", "tempo", "key", "byte_hash", "pcm_hash".
std::string_view to_string(Stage stage) noexcept;
/// Inverse of to_string(Stage); nullopt for any other text (names are case-sensitive).
std::optional<Stage> parse_stage(std::string_view text) noexcept;

/// Algorithm versions are part of every artifact identity; bump on any output-changing edit.
struct AlgorithmVersions {
    static constexpr std::string_view kWaveform = "waveform-minmaxrms-2";
    static constexpr std::string_view kLoudness = "loudness-peak-rms-1";
    static constexpr std::string_view kTempo = "tempo-flux-autocorr-2";
    static constexpr std::string_view kKey = "key-chroma-ks-2";
    static constexpr std::string_view kByteHash = "sha256-bytes-1";
    static constexpr std::string_view kPcmHash = "sha256-pcm-s24-1";
};

/// The algorithm version that produces `stage`'s results (one of AlgorithmVersions).
std::string_view algorithm_version(Stage stage) noexcept;

/// Per-bin signed peaks and RMS across all channels for an overview of the whole file.
struct WaveformOverview {
    std::vector<float> minimum;  ///< one value per bin, [-1, 1]
    std::vector<float> maximum;  ///< one value per bin, [-1, 1]
    std::vector<float> rms;      ///< one value per bin, [0, 1]
    std::int64_t frames_per_bin = 0;
    std::int64_t frames_covered = 0;  ///< decoded so far; less than total when partial
    std::int64_t total_frames = 0;    ///< 0 when unknown
    int sample_rate = 0;
    bool complete = false;  ///< the whole stream was decoded
};

/// Sample peak and plain RMS level of every sample of every channel (not LUFS).
struct LoudnessResult {
    double peak_dbfs = 0;  ///< sample peak; digital silence is reported as -200
    double rms_dbfs = 0;   ///< root mean square; a full-scale sine reads -3.01
};

/// Tempo estimate; nullopt fields mean "no reliable tempo" (one-shots, noise, ambience).
struct TempoResult {
    std::optional<std::int64_t> milli_bpm;
    double confidence = 0;  ///< heuristic in [0,1]; not a probability
};

/// Key estimate; nullopt key means non-tonal or ambiguous.
struct KeyResult {
    std::optional<MusicalKey> key;
    double confidence = 0;  ///< heuristic in [0,1]; not a probability
};

/// Content hashes. pcm_sha256 hashes decoded audio quantized to signed 24-bit together with
/// channel count and sample rate, so it is equal only for bit-identical audio content.
struct HashResult {
    std::optional<std::string> byte_sha256;
    std::optional<std::string> pcm_sha256;
};

/// Evidence captured before and after reading so the supervisor can detect INPUT_CHANGED. The
/// `mtime_ns` names match the catalog's `locations.mtime_ns` column (nanoseconds since the Unix
/// epoch of the file clock).
struct InputEvidence {
    std::int64_t size_before = 0;
    std::int64_t size_after = 0;
    std::int64_t mtime_ns_before = 0;
    std::int64_t mtime_ns_after = 0;
    /// True when size and modification time did not change while the file was read.
    bool unchanged() const { return size_before == size_after && mtime_ns_before == mtime_ns_after; }
};

/// What to analyze: one file and the stages to run on it.
struct AnalysisRequest {
    std::filesystem::path file;
    std::vector<Stage> stages;                            ///< duplicates are allowed and collapsed
    int waveform_bins = kDefaultWaveformBins;             ///< in [1, kMaximumWaveformBins]
    double max_musical_seconds = kDefaultMusicalSeconds;  ///< tempo/key analyze at most this much audio
};

/// Everything one analyze_file() call produced. Each optional is set exactly when its stage was
/// requested; an empty stream yields a waveform with zero bins.
struct AnalysisReport {
    std::optional<WaveformOverview> waveform;
    std::optional<LoudnessResult> loudness;
    std::optional<TempoResult> tempo;
    std::optional<KeyResult> key;
    HashResult hashes;
    InputEvidence evidence;
    std::int64_t frames_decoded = 0;
    std::vector<std::string> warnings;  ///< human-readable notes without file paths
};

/// Progress in [0,1]; called from the analyzing thread at a bounded rate.
using ProgressCallback = std::function<void(double fraction)>;

/// Counts completed units of work: one per decoded block of kDecodeBlockFrames frames, one per
/// hashed 1 MiB chunk. The analyzing thread increments it; a watchdog on another thread compares
/// successive readings to tell a slow analysis (the count moves) from a stuck one (it does not),
/// also for streams of unknown length, which report no fractional progress. Only changes of the
/// value matter, so relaxed ordering is enough.
using WorkCounter = std::atomic<std::uint64_t>;

/// Runs the requested stages with one shared streaming decode. Returns CANCELLED when stop is
/// requested, INPUT_CHANGED when the file's size/mtime changed while reading, and decoder errors
/// (UNSUPPORTED_CODEC, CORRUPT_AUDIO, FILE_MISSING, ACCESS_DENIED, SOURCE_OFFLINE) otherwise.
/// Memory is bounded regardless of file length. No exception escapes: allocation failure is
/// RESOURCE_LIMIT and any other exception INTERNAL.
///
/// @param work_counter optional; incremented as described at WorkCounter.
Result<AnalysisReport> analyze_file(const AnalysisRequest& request, std::stop_token stop,
                                    const ProgressCallback& progress = {}, WorkCounter* work_counter = nullptr);

/// Versioned binary encoding of a waveform overview for derived/waveforms segment files:
/// magic, version, counts, payload and SHA-256 checksum. Decoding validates every field.
std::vector<std::uint8_t> encode_waveform(const WaveformOverview& overview);
/// Size in bytes of encode_waveform()'s output for an overview with `bins` bins.
std::uint64_t encoded_waveform_size(std::size_t bins) noexcept;
/// Inverse of encode_waveform(); PARSE_ERROR for anything truncated, corrupted or inconsistent.
Result<WaveformOverview> decode_waveform(const std::vector<std::uint8_t>& bytes);

}  // namespace mso::analysis
