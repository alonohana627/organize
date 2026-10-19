#pragma once
/// @file
/// Bits of `effective_metadata.analysis_flags` (and `search::SampleRow::analysis_flags`): which
/// analysis stages have recorded a current result for a sample. The catalog sets them when it
/// records analysis, the search field `analyzed:` filters on them and the UI lists them. The values
/// are persisted in every catalog: never renumber a bit, only add new ones.
#include <cstdint>
#include <string_view>

namespace mso {

inline constexpr std::int64_t kAnalyzedTempo = 1;      ///< tempo (BPM) estimate recorded
inline constexpr std::int64_t kAnalyzedKey = 2;        ///< musical key estimate recorded
inline constexpr std::int64_t kAnalyzedWaveform = 4;   ///< waveform overview artifact recorded
inline constexpr std::int64_t kAnalyzedHash = 8;       ///< byte or PCM content hash recorded
inline constexpr std::int64_t kAnalyzedLoudness = 16;  ///< loudness measurement recorded

/// Every defined bit; `analyzed:any` matches a sample with at least one of them.
inline constexpr std::int64_t kAnalyzedAny =
    kAnalyzedTempo | kAnalyzedKey | kAnalyzedWaveform | kAnalyzedHash | kAnalyzedLoudness;

/// The bit an analysis stage sets when its result is recorded ("tempo", "key", "waveform",
/// "byte_hash", "pcm_hash", "loudness"); 0 for stages that have no flag.
constexpr std::int64_t analysis_flag_for_stage(std::string_view stage) noexcept {
    if (stage == "tempo") return kAnalyzedTempo;
    if (stage == "key") return kAnalyzedKey;
    if (stage == "waveform") return kAnalyzedWaveform;
    if (stage == "byte_hash" || stage == "pcm_hash") return kAnalyzedHash;
    if (stage == "loudness") return kAnalyzedLoudness;
    return 0;
}

}  // namespace mso
