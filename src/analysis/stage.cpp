/// @file
/// Stable text names and algorithm versions of analysis stages, used by profiles, IPC messages and
/// artifact identities.
#include <array>
#include <tuple>

#include "mso/analysis/analysis.hpp"

namespace mso::analysis {
namespace {

/// One row per stage: the stage, its stable name and the algorithm version producing it.
constexpr std::array<std::tuple<Stage, std::string_view, std::string_view>, 6> kStages{{
    {Stage::Waveform, "waveform", AlgorithmVersions::kWaveform},
    {Stage::Loudness, "loudness", AlgorithmVersions::kLoudness},
    {Stage::Tempo, "tempo", AlgorithmVersions::kTempo},
    {Stage::Key, "key", AlgorithmVersions::kKey},
    {Stage::ByteHash, "byte_hash", AlgorithmVersions::kByteHash},
    {Stage::PcmHash, "pcm_hash", AlgorithmVersions::kPcmHash},
}};

static_assert(kStages.size() == std::size(kAllStages), "every stage needs a name and an algorithm version");

}  // namespace

std::string_view to_string(Stage stage) noexcept {
    for (const auto& [candidate, name, version] : kStages)
        if (candidate == stage) return name;
    return "unknown";
}

std::optional<Stage> parse_stage(std::string_view text) noexcept {
    for (const auto& [stage, name, version] : kStages)
        if (name == text) return stage;
    return std::nullopt;
}

std::string_view algorithm_version(Stage stage) noexcept {
    for (const auto& [candidate, name, version] : kStages)
        if (candidate == stage) return version;
    return "unknown";
}

}  // namespace mso::analysis
