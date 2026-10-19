/// @file
/// Stable text names of analysis stages, used by profiles, IPC messages and artifact identities.
#include <array>
#include <utility>

#include "mso/analysis/analysis.hpp"

namespace mso::analysis {
namespace {

constexpr std::array<std::pair<Stage, std::string_view>, 6> kStageNames{{
    {Stage::Waveform, "waveform"},
    {Stage::Loudness, "loudness"},
    {Stage::Tempo, "tempo"},
    {Stage::Key, "key"},
    {Stage::ByteHash, "byte_hash"},
    {Stage::PcmHash, "pcm_hash"},
}};

}  // namespace

std::string_view to_string(Stage stage) noexcept {
    for (const auto& [candidate, name] : kStageNames)
        if (candidate == stage) return name;
    return "unknown";
}

std::optional<Stage> parse_stage(std::string_view text) noexcept {
    for (const auto& [stage, name] : kStageNames)
        if (name == text) return stage;
    return std::nullopt;
}

}  // namespace mso::analysis
