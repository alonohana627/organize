// Extension pre-filter for library enumeration.
#include <algorithm>
#include <array>

#include "mso/media/media.hpp"

namespace mso::media {

namespace {

/// Suffixes worth probing. MIDI (mid, midi) is deliberately absent: it is not audio.
constexpr std::array<std::string_view, 19> kCandidateExtensions = {
    "wav", "wave", "rf64", "bwf", "aif", "aiff", "aifc", "flac", "mp3",  "ogg",
    "oga", "opus", "m4a",  "mp4", "aac", "caf",  "wv",   "wma",  "alac",
};

}  // namespace

bool is_candidate_extension(std::string_view extension_lowercase) noexcept {
    return std::ranges::find(kCandidateExtensions, extension_lowercase) != kCandidateExtensions.end();
}

}  // namespace mso::media
