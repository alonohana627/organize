#pragma once
/// @file
/// Explicit audio analysis of one file (docs/05_AUDIO_INTELLIGENCE.md). Pure computation: no
/// catalog writes. Runs inside the mso-worker child process; results return to the supervisor,
/// which fences them by lease, cancel epoch and content revision before committing.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"

namespace mso::analysis {

/// Independently selectable analysis stages. A profile enables a subset.
enum class Stage { Waveform, Loudness, Tempo, Key, ByteHash, PcmHash };

std::string_view to_string(Stage stage) noexcept;  ///< "waveform", "loudness", "tempo", "key", "byte_hash", "pcm_hash"
std::optional<Stage> parse_stage(std::string_view text) noexcept;

/// Algorithm versions are part of every artifact identity; bump on any output-changing edit.
struct AlgorithmVersions {
    static constexpr std::string_view kWaveform = "waveform-minmaxrms-1";
    static constexpr std::string_view kLoudness = "loudness-peak-rms-1";
    static constexpr std::string_view kTempo = "tempo-flux-autocorr-1";
    static constexpr std::string_view kKey = "key-chroma-ks-1";
    static constexpr std::string_view kByteHash = "sha256-bytes-1";
    static constexpr std::string_view kPcmHash = "sha256-pcm-s24-1";
};

/// Per-bin signed peaks and RMS across all channels for an overview of the whole file.
struct WaveformOverview {
    std::vector<float> minimum;  ///< one value per bin, [-1, 1]
    std::vector<float> maximum;  ///< one value per bin, [-1, 1]
    std::vector<float> rms;      ///< one value per bin, [0, 1]
    std::int64_t frames_per_bin = 0;
    std::int64_t frames_covered = 0;  ///< decoded so far; less than total when partial
    std::int64_t total_frames = 0;    ///< 0 when unknown
    int sample_rate = 0;
    bool complete = false;
};

struct LoudnessResult {
    double peak_dbfs = 0;  ///< sample peak; -inf is reported as -200
    double rms_dbfs = 0;
};

/// Tempo estimate; nullopt fields mean "no reliable tempo" (one-shots, noise, ambience).
struct TempoResult {
    std::optional<std::int64_t> milli_bpm;
    double confidence = 0;  ///< heuristic in [0,1]; not a probability
};

/// Key estimate; nullopt key means non-tonal or ambiguous.
struct KeyResult {
    std::optional<MusicalKey> key;
    double confidence = 0;
};

/// Content hashes. pcm_sha256 hashes decoded audio quantized to signed 24-bit together with
/// channel count and sample rate, so it is equal only for bit-identical audio content.
struct HashResult {
    std::optional<std::string> byte_sha256;
    std::optional<std::string> pcm_sha256;
};

/// Evidence captured before and after reading so the supervisor can detect INPUT_CHANGED.
struct InputEvidence {
    std::int64_t size_before = 0;
    std::int64_t size_after = 0;
    std::int64_t mtime_ns_before = 0;
    std::int64_t mtime_ns_after = 0;
    bool unchanged() const { return size_before == size_after && mtime_ns_before == mtime_ns_after; }
};

struct AnalysisRequest {
    std::filesystem::path file;
    std::vector<Stage> stages;
    int waveform_bins = 2048;            ///< bounded overview resolution
    double max_musical_seconds = 120.0;  ///< tempo/key analyze at most this much audio
};

struct AnalysisReport {
    std::optional<WaveformOverview> waveform;
    std::optional<LoudnessResult> loudness;
    std::optional<TempoResult> tempo;
    std::optional<KeyResult> key;
    HashResult hashes;
    InputEvidence evidence;
    std::int64_t frames_decoded = 0;
    std::vector<std::string> warnings;
};

/// Progress in [0,1]; called from the analyzing thread at a bounded rate.
using ProgressCallback = std::function<void(double fraction)>;

/// Runs the requested stages with one shared streaming decode. Returns CANCELLED when stop is
/// requested, INPUT_CHANGED when the file's size/mtime changed while reading, and decoder errors
/// (UNSUPPORTED_CODEC, CORRUPT_AUDIO, FILE_MISSING) otherwise. Memory is bounded regardless of
/// file length.
Result<AnalysisReport> analyze_file(const AnalysisRequest& request, std::stop_token stop,
                                    const ProgressCallback& progress = {});

/// Versioned binary encoding of a waveform overview for derived/waveforms segment files:
/// magic, version, counts, payload and SHA-256 checksum. Decoding validates every field.
std::vector<std::uint8_t> encode_waveform(const WaveformOverview& overview);
Result<WaveformOverview> decode_waveform(const std::vector<std::uint8_t>& bytes);

}  // namespace mso::analysis
