#pragma once
/// @file
/// Value types exchanged with the catalog. Plain data, no SQLite or Qt types.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"

namespace mso::catalog {

/// How a root's audio is owned: referenced in place, or copied into the library's media folder.
enum class RootMode { Referenced, Managed };

/// Last observed reachability of a root (catalog projection; live status may be newer).
enum class Availability { Online, Offline, Unknown, Denied };

/// State of one file occurrence.
enum class LocationState { Present, Missing, Unknown, Denied, Changed, Retired };

std::string_view to_string(RootMode mode) noexcept;
std::string_view to_string(Availability availability) noexcept;
std::string_view to_string(LocationState state) noexcept;
std::optional<RootMode> parse_root_mode(std::string_view text) noexcept;
std::optional<Availability> parse_availability(std::string_view text) noexcept;
std::optional<LocationState> parse_location_state(std::string_view text) noexcept;

/// Identity and revision of an open library.
struct LibraryIdentity {
    uuids::uuid uuid;
    std::string name;
    int schema_version = 0;
    std::int64_t catalog_revision = 0;
};

/// A source directory registered in the library. Its absolute path is a machine-local binding
/// kept by the workspace, never in the portable catalog.
struct RootRecord {
    std::int64_t id = 0;
    uuids::uuid uuid;
    std::string name;
    RootMode mode = RootMode::Referenced;
    Availability availability = Availability::Unknown;
    std::int64_t scan_generation = 0;
    std::optional<std::int64_t> last_seen_at;
    std::int64_t sample_count = 0;
};

/// A folder below a root, for folder navigation.
struct FolderRecord {
    std::int64_t id = 0;
    std::int64_t root_id = 0;
    std::optional<std::int64_t> parent_id;
    std::string relative_path;  ///< "" for the root folder itself
    std::string name;           ///< last path segment
};

/// A node of the tag or category tree. `path` is the display path, e.g. "Drums/Kick".
struct TaxonomyNode {
    std::int64_t id = 0;
    uuids::uuid uuid;
    std::optional<std::int64_t> parent_id;
    std::string name;
    std::string path;
    std::int64_t sample_count = 0;
    std::optional<std::int64_t> alias_of;  ///< tags only: merged into this canonical tag
};

/// A manual (ordered membership) or smart (stored query) collection.
struct CollectionRecord {
    std::int64_t id = 0;
    uuids::uuid uuid;
    std::string name;
    bool smart = false;
    std::optional<std::string> query;  ///< smart collections: the query text
    std::int64_t sample_count = 0;     ///< manual collections only
};

/// Metadata observed by a bounded import probe plus filename hints, ready to be committed.
/// Produced by the files module from a media::ProbeResult; the catalog does not depend on media.
struct ObservedMetadata {
    std::string format;
    std::string codec;
    std::optional<std::int64_t> sample_rate;
    std::optional<std::int64_t> channels;
    std::optional<std::int64_t> bit_depth;
    std::optional<std::int64_t> frames;
    std::optional<std::int64_t> duration_us;
    bool duration_exact = false;
    bool partial = false;
    std::string embedded_text;  ///< title/artist/genre/comment/keywords joined for search
    std::optional<std::int64_t> embedded_bpm_milli;
    std::optional<MusicalKey> embedded_key;
    std::optional<std::int64_t> filename_bpm_milli;
    std::optional<MusicalKey> filename_key;
    std::int64_t loop_count = 0;
};

/// One file found by enumeration, after stat and (optionally) a bounded probe.
struct ScannedFile {
    std::string relative_path;  ///< '/'-separated, validated with is_safe_relative_path
    std::int64_t byte_size = 0;
    std::int64_t mtime_ns = 0;
    std::optional<std::string> volume_id;  ///< volume serial, hex
    std::optional<std::string> file_id;    ///< 128-bit file id, hex
    std::optional<ObservedMetadata> metadata;
    std::optional<ErrorCode> probe_error;  ///< probe failed: record stays searchable by name/path
};

/// Handle for a running scan of one root.
struct ScanSession {
    std::int64_t id = 0;
    std::int64_t root_id = 0;
    std::int64_t generation = 0;
};

/// Counts reported for a committed scan batch.
struct ScanBatchOutcome {
    std::int64_t added = 0;
    std::int64_t updated = 0;  ///< content possibly changed: revision bumped, analysis invalidated
    std::int64_t unchanged = 0;
    std::int64_t relinked = 0;  ///< same-volume rename recognized by file identity
};

/// Result of finishing a scan.
struct ScanFinishOutcome {
    std::int64_t marked_missing = 0;
    bool complete_coverage = false;
};

/// One file occurrence of a sample.
struct LocationRecord {
    std::int64_t id = 0;
    std::int64_t root_id = 0;
    std::string root_name;
    uuids::uuid root_uuid;
    RootMode root_mode = RootMode::Referenced;
    std::string relative_path;
    LocationState state = LocationState::Unknown;
    std::optional<std::int64_t> byte_size;
    std::optional<std::int64_t> mtime_ns;
    std::optional<std::string> byte_hash_hex;
};

/// A recorded fact with provenance (container tag, filename heuristic, analysis...).
struct ObservationRecord {
    std::string field;
    std::string value_json;
    std::string source;
    std::optional<std::string> algorithm_version;
    std::optional<double> confidence;
    std::int64_t content_revision = 0;
    bool valid = true;
    std::int64_t created_at = 0;
};

/// Override state for user-editable musical facts (BPM, key).
enum class OverrideState { Automatic, Value, Unknown };
std::string_view to_string(OverrideState state) noexcept;
std::optional<OverrideState> parse_override_state(std::string_view text) noexcept;

/// Everything the inspector shows about one sample.
struct SampleDetail {
    std::int64_t id = 0;
    uuids::uuid uuid;
    std::string display_name;
    std::string original_name;
    std::int64_t content_revision = 1;
    std::int64_t edit_revision = 0;
    bool favorite = false;
    int rating = 0;
    std::string notes;
    std::optional<std::int64_t> category_id;
    std::string category_path;
    std::vector<TaxonomyNode> tags;
    std::vector<CollectionRecord> collections;
    OverrideState bpm_override = OverrideState::Automatic;
    std::optional<std::int64_t> bpm_override_milli;
    OverrideState key_override = OverrideState::Automatic;
    std::optional<MusicalKey> key_override_value;
    // effective metadata
    std::string format, codec;
    std::optional<std::int64_t> sample_rate, channels, bit_depth, frames, duration_us, byte_size;
    bool duration_exact = false;
    std::optional<std::int64_t> bpm_milli;
    std::string bpm_source;
    std::optional<double> bpm_confidence;
    std::optional<MusicalKey> key;
    std::string key_source;
    std::optional<double> key_confidence;
    std::vector<LocationRecord> locations;
    std::vector<ObservationRecord> observations;
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
    bool deleted = false;
};

/// What an authoritative commit produced (docs/04_CONTRACTS.md §1).
struct MutationReceipt {
    uuids::uuid operation;
    std::int64_t committed_revision = 0;
    std::optional<uuids::uuid> history_action;
    std::int64_t applied = 0;
    std::int64_t skipped = 0;
    std::int64_t conflicted = 0;
    bool replayed = false;  ///< the same operation was already committed; stored receipt returned
};

/// One user-visible history entry.
struct HistoryEntry {
    std::int64_t id = 0;
    uuids::uuid uuid;
    std::string label;
    std::string state;  ///< applied, partial, undone, superseded, conflict
    std::int64_t created_at = 0;
    bool is_cursor = false;  ///< the next Undo would revert this action
};

/// An item awaiting user review (ambiguous relink, history conflict, content change...).
struct ReviewItem {
    std::int64_t id = 0;
    uuids::uuid uuid;
    std::string kind;
    std::optional<std::int64_t> sample_id;
    std::string evidence_json;
    std::string state;
    std::int64_t created_at = 0;
};

}  // namespace mso::catalog
