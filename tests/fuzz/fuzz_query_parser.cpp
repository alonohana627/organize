// libFuzzer target: the search query language (include/mso/search/query.hpp) on malformed text.
//
// Query text comes from the search box, the command line and saved smart collections. The parser must
// reject anything malformed with PARSE_ERROR and never crash, and an accepted query must respect the
// documented node limit. Every accepted query is then evaluated by the in-memory
// RowMatcher (the path the desktop's index uses) against two fixed rows, one with every field empty
// and one with every field set, so value parsing, normalization and the matcher's caches see the
// parsed values too.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mso/search/query.hpp"
#include "mso/search/row_matcher.hpp"
#include "mso/search/sample_rows.hpp"

namespace {

/// Counts the nodes below and including `node`.
std::size_t count_nodes(const mso::search::QueryNode& node) {
    std::size_t nodes = 1;
    for (const auto& child : node.children)
        nodes += count_nodes(*child);
    return nodes;
}

/// A row with every searchable field set to a plausible value.
mso::search::SampleRow full_row() {
    mso::search::SampleRow row;
    row.sample_id = 7;
    row.display_name = "Kick Hard 01";
    row.file_name = "kick_hard_01.wav";
    row.root_id = 1;
    row.root_name = "Samples";
    row.relative_path = "Drums/Kicks/kick_hard_01.wav";
    row.path_key = "drums/kicks/kick_hard_01.wav";
    row.folder_path = "Drums/Kicks";
    row.format = "wav";
    row.codec = "pcm_s16";
    row.duration_us = 1'250'000;
    row.duration_exact = true;
    row.sample_rate = 44'100;
    row.channels = 2;
    row.bit_depth = 16;
    row.byte_size = 220'544;
    row.bpm_milli = 128'000;
    row.bpm_source = "analysis";
    row.bpm_confidence = 0.9;
    row.key = mso::MusicalKey{9, mso::KeyMode::Minor};
    row.key_source = "analysis";
    row.key_confidence = 0.8;
    row.category_id = 3;
    row.category = "Drums/Kicks";
    row.tags = "Drums/Kick, Mood/Dark";
    row.tag_paths = {"Drums/Kick", "Mood/Dark"};
    row.tag_ids = {4, 5};
    row.rating = 4;
    row.favorite = true;
    row.availability = mso::search::RowAvailability::Available;
    row.analysis_flags = 0x7;
    row.created_at = 1'700'000'000'000;
    return row;
}

}  // namespace

/// libFuzzer entry point: parses `size` bytes at `data` as query text and evaluates what parses.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto query = mso::search::parse_query(text);
    if (!query) return 0;
    // The documented node limit holds for every accepted query (the depth limit counts parentheses and
    // NOT chains, which the tree does not record, so it is left to the parser's own tests).
    if (!query->empty() && count_nodes(*query->root) > mso::search::kMaxQueryNodes) __builtin_trap();
    static const mso::search::SampleRow empty_row{};
    static const mso::search::SampleRow populated_row = full_row();
    const mso::search::RowMatcher matcher(*query);
    [[maybe_unused]] const bool empty_matches = matcher(empty_row);
    [[maybe_unused]] const bool populated_matches = matcher(populated_row);
    [[maybe_unused]] const auto terms = mso::search::positive_text_terms(*query);
    return 0;
}
