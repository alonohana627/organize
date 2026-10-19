#pragma once
/// @file
/// Hydrated sample rows: everything the table, inspector, exports and the application's in-memory
/// index show about one sample, loaded set-based (a handful of joined queries per call, never one
/// query per row).
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mso/catalog/library.hpp"

namespace mso::search {

/// Playability of a sample as displayed in the table.
enum class RowAvailability {
    Available,      ///< primary location present on a reachable root
    SourceOffline,  ///< present, but its root is offline (or removed)
    Missing,        ///< the file was not found at its last scan
    Unknown,        ///< no live location, or a state that is neither present nor missing
};
/// Display name: "available", "source offline", "missing" or "unknown".
std::string_view to_string(RowAvailability availability) noexcept;

/// One hydrated result row. Location fields describe the sample's *primary* location: its live
/// location ranked present-on-a-reachable-root first, then present, then oldest
/// (detail::kPrimaryLocationOrder); `folder:` and the root/folder scopes test the same location.
struct SampleRow {
    uuids::uuid library;                                          ///< owning library
    std::int64_t sample_id = 0;                                   ///< library-local id
    uuids::uuid sample_uuid;                                      ///< stable sample identity
    std::string display_name;                                     ///< user-visible name
    std::string file_name;                                        ///< last segment of `relative_path`
    std::int64_t root_id = 0;                                     ///< root of the primary location (0 = none)
    std::string root_name;                                        ///< that root's display name
    uuids::uuid root_uuid;                                        ///< that root's identity
    catalog::RootMode root_mode = catalog::RootMode::Referenced;  ///< referenced or managed root
    bool root_case_sensitive = false;                             ///< whether the root compares paths exactly
    std::string relative_path;                                    ///< primary location, root-relative, '/'-separated
    std::string folder_path;                                      ///< root-relative directory of it ("" at the root)
    std::string format;                                       ///< container format ("wav", "flac"...), "" when unknown
    std::string codec;                                        ///< codec name, "" when unknown
    std::optional<std::int64_t> duration_us;                  ///< duration in microseconds
    bool duration_exact = false;                              ///< duration measured rather than estimated
    std::optional<std::int64_t> sample_rate;                  ///< Hz
    std::optional<std::int64_t> channels;                     ///< channel count
    std::optional<std::int64_t> bit_depth;                    ///< bits per sample
    std::optional<std::int64_t> byte_size;                    ///< file size in bytes
    std::optional<std::int64_t> bpm_milli;                    ///< tempo in milli-BPM
    std::string bpm_source;                                   ///< where the tempo came from
    std::optional<double> bpm_confidence;                     ///< 0..1 for analysis results
    std::optional<MusicalKey> key;                            ///< musical key
    std::string key_source;                                   ///< where the key came from
    std::optional<double> key_confidence;                     ///< 0..1 for analysis results
    std::optional<std::int64_t> category_id;                  ///< assigned category, if any
    std::string category;                                     ///< its display path ("Drums/Kicks"), "" when none
    std::string tags;                                         ///< tag display paths joined with ", "
    std::vector<std::string> tag_paths;                       ///< the same paths as a list, sorted
    std::vector<std::int64_t> tag_ids;                        ///< tag ids, parallel to `tag_paths`
    int rating = 0;                                           ///< 0..5 stars
    bool favorite = false;                                    ///< favorite flag
    RowAvailability availability = RowAvailability::Unknown;  ///< playability of the primary location
    std::int64_t analysis_flags = 0;                          ///< kAnalyzed* bits (domain/analysis_flags.hpp)
    std::int64_t created_at = 0;                              ///< Unix milliseconds the sample was added
    double relevance = 0;                                     ///< relevance sort only: fused rank score
};

/// Set-based hydration of specific samples, in the order given (duplicates once). Deleted or
/// unknown ids are simply absent from the result.
Result<std::vector<SampleRow>> load_rows(catalog::Library& library, std::span<const std::int64_t> sample_ids);

/// Streams every live sample row of a library in ascending id order, set-based: one ordered pass
/// over samples, locations and tag links merged in memory, with the taxonomy and roots read once.
/// Runs on one read snapshot. `visit` must not call back into this library's reader pool.
Result<void> for_each_row(catalog::Library& library, const std::function<void(SampleRow&&)>& visit);

}  // namespace mso::search
