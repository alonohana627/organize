#include "mso/app/sample_index.hpp"

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "mso/domain/json_fields.hpp"
#include "mso/search/row_matcher.hpp"

namespace mso::app {

namespace {

using search::SortField;

/// Cursor version of index keyset cursors. SearchService cursors use 1 and the earlier offset
/// cursors of the index used 101, so each rejects the others' tokens as STALE_CURSOR.
constexpr std::int64_t kIndexCursorVersion = 102;
/// Number of SortField values (Relevance shares Name's order).
constexpr std::size_t kSortFieldCount = 10;
/// Minimum spacing of outbox acknowledgements (each one is a small catalog write).
constexpr auto kAcknowledgeInterval = std::chrono::seconds(5);
/// Above this many changed rows a cached sort order is dropped and rebuilt on demand instead of
/// being patched in place. A patch costs one pass over the order plus a binary search per changed
/// row; a rebuild sorts every row with key comparisons, which costs far more up to about this size.
constexpr std::size_t kMaximumPatchedRows = 32'768;
/// SQL id sets kept per index (full-text terms, collection membership).
constexpr std::size_t kCachedIdSets = 4;
/// Depth limit of tag subtrees, as in the SQL scope compiler.
constexpr int kMaximumTaxonomyDepth = 16;
/// Slot map marker of an id without a row.
constexpr std::uint32_t kNoSlot = std::numeric_limits<std::uint32_t>::max();
/// Sample ids are dense SQLite row ids, so the id → slot map and the SQL id sets are vectors indexed
/// by id. Their size is tied to the row limit: ids up to kIdsPerRowLimit times the row limit (and at
/// least kMinimumIdLimit) are indexed; a library with larger ids (a long history of removed samples)
/// is answered from SQLite instead, so a sparse or damaged id never commits gigabytes.
constexpr std::int64_t kIdsPerRowLimit = 4;
constexpr std::int64_t kMinimumIdLimit = std::int64_t{1} << 20;
/// Hard ceiling of the id limit (slots are 32-bit).
constexpr std::int64_t kMaximumIdLimit = std::int64_t{1} << 31;

/// The largest sample id (exclusive) an index loaded with `row_limit` holds.
std::int64_t id_limit_for(std::size_t row_limit) {
    const auto rows = static_cast<std::int64_t>(std::min<std::size_t>(row_limit, kMaximumIdLimit / kIdsPerRowLimit));
    return std::clamp(rows * kIdsPerRowLimit, kMinimumIdLimit, kMaximumIdLimit);
}

// ---------------------------------------------------------------- shared text

/// Transparent hash so maps keyed by text can be probed with a std::string_view.
struct TextHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
};

/// Text that many rows share (root names, formats, codecs, value sources, category paths), stored
/// once and referred to by number. Number 0 is the empty text. Only grows; a reload starts afresh.
class TextPool {
public:
    TextPool() { ids_.emplace(texts_.emplace_back(), 0); }
    TextPool(TextPool&&) noexcept = default;  // a deque's elements keep their addresses when it moves
    TextPool& operator=(TextPool&&) noexcept = default;
    TextPool(const TextPool&) = delete;
    TextPool& operator=(const TextPool&) = delete;
    ~TextPool() = default;

    /// The number of `text`, adding it when new.
    std::uint32_t intern(std::string_view text) {
        if (const auto found = ids_.find(text); found != ids_.end()) return found->second;
        const auto id = static_cast<std::uint32_t>(texts_.size());
        ids_.emplace(texts_.emplace_back(text), id);
        return id;
    }
    /// The text of a number returned by intern().
    const std::string& text(std::uint32_t id) const { return texts_[id]; }

private:
    std::deque<std::string> texts_;  ///< stable addresses: the map's keys view them
    std::unordered_map<std::string_view, std::uint32_t> ids_;
};

/// A root as rows reference it.
struct RootEntry {
    std::int64_t root_id = 0;  ///< 0 = the sample has no live location
    std::uint32_t name = 0;    ///< TextPool number of the display name
    uuids::uuid uuid;
    catalog::RootMode mode = catalog::RootMode::Referenced;
    bool case_sensitive = false;
    bool operator==(const RootEntry&) const = default;
};

/// A folder of a root as rows reference it, with its location key (the catalog's path_key of the
/// folder: folded unless the root is case-sensitive).
struct FolderEntry {
    std::uint32_t root = 0;  ///< RootEntry number
    std::string path;        ///< root-relative, "" for the root itself
    std::string key;         ///< path_key(path, root case sensitivity)
};

/// The tags of a row; rows with the same tags share one entry.
struct TagSet {
    std::vector<std::string> paths;  ///< SampleRow::tag_paths
    std::vector<std::int64_t> ids;   ///< SampleRow::tag_ids
};

/// Everything rows refer to by number. Entry 0 of every table is the empty value.
struct SharedTables {
    TextPool texts;
    std::vector<RootEntry> roots{RootEntry{}};
    std::vector<FolderEntry> folders{FolderEntry{}};
    /// Per root number: folder path → folder number.
    std::vector<std::unordered_map<std::string, std::uint32_t, TextHash, std::equal_to<>>> folder_numbers{1};
    std::vector<TagSet> tag_sets{TagSet{}};
    std::unordered_map<std::string, std::uint32_t> tag_set_numbers;

    /// The number of a row's root (0 when it has no live location).
    std::uint32_t intern_root(const search::SampleRow& row) {
        if (row.root_id == 0) return 0;
        const RootEntry wanted{row.root_id, texts.intern(row.root_name), row.root_uuid, row.root_mode,
                               row.root_case_sensitive};
        for (std::uint32_t number = 1; number < roots.size(); ++number)
            if (roots[number] == wanted) return number;
        roots.push_back(wanted);
        folder_numbers.emplace_back();
        return static_cast<std::uint32_t>(roots.size() - 1);
    }

    /// The number of a folder of a root, computing its key when new.
    std::uint32_t intern_folder(std::uint32_t root, std::string_view path) {
        auto& numbers = folder_numbers[root];
        if (const auto found = numbers.find(path); found != numbers.end()) return found->second;
        const auto number = static_cast<std::uint32_t>(folders.size());
        folders.push_back(FolderEntry{root, std::string(path), path_key(path, roots[root].case_sensitive)});
        numbers.emplace(std::string(path), number);
        return number;
    }

    /// The number of a row's tag set.
    std::uint32_t intern_tags(const search::SampleRow& row) {
        if (row.tag_paths.empty() && row.tag_ids.empty()) return 0;
        std::string signature;
        for (const auto id : row.tag_ids)
            signature += std::format("\x1f{}", id);
        for (const auto& path : row.tag_paths)
            signature += "\x1e" + path;
        if (const auto found = tag_set_numbers.find(signature); found != tag_set_numbers.end()) return found->second;
        const auto number = static_cast<std::uint32_t>(tag_sets.size());
        tag_sets.push_back(TagSet{row.tag_paths, row.tag_ids});
        tag_set_numbers.emplace(std::move(signature), number);
        return number;
    }
};

// ---------------------------------------------------------------- compact rows

/// The three texts of one row (display name, name key, file name) in a single heap block. Three
/// std::string members cost 72 bytes inline plus one allocation each once a name outgrows libc++'s
/// 22-byte small-string buffer, which real pack names (25-60 bytes, often not ASCII) do; one block
/// costs a pointer and one allocation. The name key is stored only when it differs from the display
/// name, and the file name only past the prefix it shares with the display name ("Kick 01" and
/// "Kick 01.wav" store ".wav").
class RowText {
public:
    RowText() = default;
    /// Packs the three texts. Each is shorter than 4 GiB (they come from file names and paths).
    RowText(std::string_view display_name, std::string_view name_key, std::string_view file_name) {
        const bool key_is_name = name_key == display_name;
        const auto shared =
            static_cast<std::size_t>(std::ranges::mismatch(display_name, file_name).in1 - display_name.begin());
        const Header header{static_cast<std::uint32_t>(display_name.size()),
                            key_is_name ? kSameAsName : static_cast<std::uint32_t>(name_key.size()),
                            static_cast<std::uint32_t>(shared), static_cast<std::uint32_t>(file_name.size() - shared)};
        const std::size_t key_bytes = key_is_name ? 0 : name_key.size();
        block_ = std::make_unique_for_overwrite<char[]>(sizeof(Header) + display_name.size() + key_bytes +
                                                        header.file_suffix);
        char* cursor = block_.get();
        std::memcpy(cursor, &header, sizeof(Header));
        cursor += sizeof(Header);
        std::ranges::copy(display_name, cursor);
        cursor += display_name.size();
        if (!key_is_name) std::ranges::copy(name_key, cursor);
        cursor += key_bytes;
        std::ranges::copy(file_name.substr(shared), cursor);
    }

    /// SampleRow::display_name.
    std::string_view display_name() const {
        if (!block_) return {};
        return {block_.get() + sizeof(Header), header().display_name};
    }
    /// fold_key(display_name): the catalog's samples.name_key, the Name sort order.
    std::string_view name_key() const {
        if (!block_) return {};
        const auto layout = header();
        if (layout.name_key == kSameAsName) return display_name();
        return {block_.get() + sizeof(Header) + layout.display_name, layout.name_key};
    }
    /// Appends the file name (or the whole relative path of a row with kVerbatimPath) to `out`.
    void append_file_name(std::string& out) const {
        if (!block_) return;
        const auto layout = header();
        out.append(display_name().substr(0, layout.file_prefix));
        const std::size_t key_bytes = layout.name_key == kSameAsName ? 0 : layout.name_key;
        out.append(block_.get() + sizeof(Header) + layout.display_name + key_bytes, layout.file_suffix);
    }
    /// The file name (or verbatim relative path) as a string.
    std::string file_name() const {
        std::string name;
        append_file_name(name);
        return name;
    }

private:
    /// Lengths at the start of the block (copied out with memcpy: the block has no alignment).
    struct Header {
        std::uint32_t display_name = 0;
        std::uint32_t name_key = 0;     ///< kSameAsName when the key equals the display name
        std::uint32_t file_prefix = 0;  ///< leading bytes the file name shares with the display name
        std::uint32_t file_suffix = 0;  ///< bytes of the file name stored after the texts
    };
    static constexpr std::uint32_t kSameAsName = std::numeric_limits<std::uint32_t>::max();

    Header header() const {
        Header layout;
        std::memcpy(&layout, block_.get(), sizeof(Header));
        return layout;
    }

    std::unique_ptr<char[]> block_;
};

/// Presence and boolean bits of a CompactRow.
enum RowBit : std::uint16_t {
    kHasDuration = 1u << 0,
    kHasSampleRate = 1u << 1,
    kHasChannels = 1u << 2,
    kHasBitDepth = 1u << 3,
    kHasByteSize = 1u << 4,
    kHasBpm = 1u << 5,
    kHasBpmConfidence = 1u << 6,
    kHasKeyConfidence = 1u << 7,
    kHasKey = 1u << 8,
    kHasCategory = 1u << 9,
    kDurationExact = 1u << 10,
    kFavorite = 1u << 11,
    kVerbatimPath = 1u << 12,  ///< the row's file name holds the whole relative path (it did not split cleanly)
};

/// One indexed sample: a search::SampleRow without the text other rows share (that lives in
/// SharedTables) and without the parts derivable from the rest (library, folder and full path).
struct CompactRow {
    std::int64_t sample_id = 0;
    std::int64_t created_at = 0;
    std::int64_t analysis_flags = 0;
    std::int64_t duration_us = 0;
    std::int64_t sample_rate = 0;
    std::int64_t channels = 0;
    std::int64_t bit_depth = 0;
    std::int64_t byte_size = 0;
    std::int64_t bpm_milli = 0;
    std::int64_t category_id = 0;
    double bpm_confidence = 0;
    double key_confidence = 0;
    uuids::uuid sample_uuid;
    /// Display name, name key (the Name order) and file name: the last path segment, or the whole
    /// relative path with kVerbatimPath.
    RowText text;
    std::uint32_t folder = 0;  ///< FolderEntry number (which also gives the root)
    std::uint32_t format = 0;  ///< TextPool numbers
    std::uint32_t codec = 0;
    std::uint32_t bpm_source = 0;
    std::uint32_t key_source = 0;
    std::uint32_t category = 0;
    std::uint32_t tag_set = 0;  ///< TagSet number
    int rating = 0;
    std::uint16_t bits = 0;  ///< RowBit flags
    std::int8_t key_pitch_class = 0;
    KeyMode key_mode = KeyMode::Unknown;
    search::RowAvailability availability = search::RowAvailability::Unknown;
    bool live = false;  ///< false for a free slot

    /// True when `bit` is set.
    bool has(RowBit bit) const { return (bits & bit) != 0; }
    /// Sets or clears `bit`.
    void set(RowBit bit, bool value) { bits = static_cast<std::uint16_t>(value ? bits | bit : bits & ~bit); }
    /// An optional column stored as value + presence bit.
    std::optional<std::int64_t> optional(std::int64_t value, RowBit bit) const {
        return has(bit) ? std::optional(value) : std::nullopt;
    }
};

/// A hydrated row with its name key, computed before any index lock is taken.
struct PreparedRow {
    search::SampleRow row;
    std::string name_key;
};

/// Folds names outside the locks (the expensive part of turning a SampleRow into a CompactRow).
std::vector<PreparedRow> prepare_rows(std::vector<search::SampleRow>&& rows) {
    std::vector<PreparedRow> prepared;
    prepared.reserve(rows.size());
    for (auto& row : rows) {
        auto name_key = fold_key(row.display_name);
        prepared.push_back(PreparedRow{std::move(row), std::move(name_key)});
    }
    return prepared;
}

/// Stores a row's value of an optional column and its presence bit.
void store_optional(CompactRow& compact, std::int64_t& field, const std::optional<std::int64_t>& value, RowBit bit) {
    field = value.value_or(0);
    compact.set(bit, value.has_value());
}

/// The compact form of a hydrated row; shared text goes into `tables`.
CompactRow compact(PreparedRow&& prepared, SharedTables& tables) {
    auto& row = prepared.row;
    CompactRow compact;
    compact.sample_id = row.sample_id;
    compact.created_at = row.created_at;
    compact.analysis_flags = row.analysis_flags;
    store_optional(compact, compact.duration_us, row.duration_us, kHasDuration);
    store_optional(compact, compact.sample_rate, row.sample_rate, kHasSampleRate);
    store_optional(compact, compact.channels, row.channels, kHasChannels);
    store_optional(compact, compact.bit_depth, row.bit_depth, kHasBitDepth);
    store_optional(compact, compact.byte_size, row.byte_size, kHasByteSize);
    store_optional(compact, compact.bpm_milli, row.bpm_milli, kHasBpm);
    store_optional(compact, compact.category_id, row.category_id, kHasCategory);
    compact.bpm_confidence = row.bpm_confidence.value_or(0);
    compact.set(kHasBpmConfidence, row.bpm_confidence.has_value());
    compact.key_confidence = row.key_confidence.value_or(0);
    compact.set(kHasKeyConfidence, row.key_confidence.has_value());
    if (row.key) {
        compact.key_pitch_class = static_cast<std::int8_t>(row.key->pitch_class);
        compact.key_mode = row.key->mode;
    }
    compact.set(kHasKey, row.key.has_value());
    compact.set(kDurationExact, row.duration_exact);
    compact.set(kFavorite, row.favorite);
    compact.sample_uuid = row.sample_uuid;
    const auto root = tables.intern_root(row);
    compact.folder = tables.intern_folder(root, row.folder_path);
    // The path is stored as folder number + file name when it splits back exactly.
    const std::string rebuilt = row.folder_path.empty() ? row.file_name : row.folder_path + '/' + row.file_name;
    const bool splits = rebuilt == row.relative_path;
    compact.set(kVerbatimPath, !splits);
    compact.text = RowText(row.display_name, prepared.name_key, splits ? row.file_name : row.relative_path);
    compact.format = tables.texts.intern(row.format);
    compact.codec = tables.texts.intern(row.codec);
    compact.bpm_source = tables.texts.intern(row.bpm_source);
    compact.key_source = tables.texts.intern(row.key_source);
    compact.category = tables.texts.intern(row.category);
    compact.tag_set = tables.intern_tags(row);
    compact.rating = row.rating;
    compact.availability = row.availability;
    compact.live = true;
    return compact;
}

/// The file name and relative path of a compact row.
std::pair<std::string, std::string> file_and_path(const CompactRow& compact, const FolderEntry& folder) {
    auto stored = compact.text.file_name();
    if (compact.has(kVerbatimPath)) {
        const auto slash = stored.rfind('/');
        return {slash == std::string::npos ? stored : stored.substr(slash + 1), stored};
    }
    auto path = folder.path.empty() ? stored : folder.path + '/' + stored;
    return {std::move(stored), std::move(path)};
}

/// The full search::SampleRow of a compact row (the inverse of compact()).
search::SampleRow expand(const CompactRow& compact, const SharedTables& tables, const uuids::uuid& library) {
    const auto& folder = tables.folders[compact.folder];
    const auto& root = tables.roots[folder.root];
    const auto& tags = tables.tag_sets[compact.tag_set];
    search::SampleRow row;
    row.library = library;
    row.sample_id = compact.sample_id;
    row.sample_uuid = compact.sample_uuid;
    row.display_name = compact.text.display_name();
    auto [file_name, relative_path] = file_and_path(compact, folder);
    row.file_name = std::move(file_name);
    row.root_id = root.root_id;
    row.root_name = tables.texts.text(root.name);
    row.root_uuid = root.uuid;
    row.root_mode = root.mode;
    row.root_case_sensitive = root.case_sensitive;
    // path_key works segment by segment, so the folder's key joins the file's key exactly.
    if (root.root_id != 0)
        row.path_key = compact.has(kVerbatimPath) || folder.path.empty()
                           ? path_key(relative_path, root.case_sensitive)
                           : folder.key + '/' + path_key(row.file_name, root.case_sensitive);
    row.relative_path = std::move(relative_path);
    row.folder_path = folder.path;
    row.format = tables.texts.text(compact.format);
    row.codec = tables.texts.text(compact.codec);
    row.duration_us = compact.optional(compact.duration_us, kHasDuration);
    row.duration_exact = compact.has(kDurationExact);
    row.sample_rate = compact.optional(compact.sample_rate, kHasSampleRate);
    row.channels = compact.optional(compact.channels, kHasChannels);
    row.bit_depth = compact.optional(compact.bit_depth, kHasBitDepth);
    row.byte_size = compact.optional(compact.byte_size, kHasByteSize);
    row.bpm_milli = compact.optional(compact.bpm_milli, kHasBpm);
    row.bpm_source = tables.texts.text(compact.bpm_source);
    if (compact.has(kHasBpmConfidence)) row.bpm_confidence = compact.bpm_confidence;
    if (compact.has(kHasKey)) row.key = MusicalKey{compact.key_pitch_class, compact.key_mode};
    row.key_source = tables.texts.text(compact.key_source);
    if (compact.has(kHasKeyConfidence)) row.key_confidence = compact.key_confidence;
    row.category_id = compact.optional(compact.category_id, kHasCategory);
    row.category = tables.texts.text(compact.category);
    row.tag_paths = tags.paths;
    row.tag_ids = tags.ids;
    row.rating = compact.rating;
    row.favorite = compact.has(kFavorite);
    row.availability = compact.availability;
    row.analysis_flags = compact.analysis_flags;
    row.created_at = compact.created_at;
    row.relevance = 0;
    return row;
}

/// Which row fields a query reads beyond the scalars (they cost a string copy per evaluated row).
struct MatchedFields {
    bool tags = false;
    bool category = false;
    bool folder = false;
    bool format = false;
    bool codec = false;
};

/// Collects the fields `node` and its children read.
void collect_fields(const search::QueryNode& node, MatchedFields& fields) {
    if (node.kind == search::QueryNode::Kind::Predicate) {
        switch (node.predicate.field) {
            case search::Field::Tag:
                fields.tags = true;
                break;
            case search::Field::Category:
                fields.category = true;
                break;
            case search::Field::Folder:
                fields.folder = true;
                break;
            case search::Field::Format:
                fields.format = true;
                break;
            case search::Field::Codec:
                fields.codec = true;
                break;
            default:
                break;
        }
    }
    for (const auto& child : node.children)
        collect_fields(*child, fields);
}

/// Fills `scratch` with what a query's evaluation reads of `compact`. The scratch row is reused
/// across rows, so its strings keep their capacity and filling allocates almost never.
/// `path_key` gets the folder's key followed by the unfolded file name: a `folder:` rule tests only
/// prefixes that end in '/', which never reach into the file name.
void fill_for_matching(search::SampleRow& scratch, const CompactRow& compact, const SharedTables& tables,
                       const MatchedFields& fields) {
    const auto& folder = tables.folders[compact.folder];
    const auto& root = tables.roots[folder.root];
    scratch.sample_id = compact.sample_id;
    scratch.root_id = root.root_id;
    scratch.root_case_sensitive = root.case_sensitive;
    scratch.duration_us = compact.optional(compact.duration_us, kHasDuration);
    scratch.sample_rate = compact.optional(compact.sample_rate, kHasSampleRate);
    scratch.channels = compact.optional(compact.channels, kHasChannels);
    scratch.bit_depth = compact.optional(compact.bit_depth, kHasBitDepth);
    scratch.byte_size = compact.optional(compact.byte_size, kHasByteSize);
    scratch.bpm_milli = compact.optional(compact.bpm_milli, kHasBpm);
    scratch.key =
        compact.has(kHasKey) ? std::optional(MusicalKey{compact.key_pitch_class, compact.key_mode}) : std::nullopt;
    scratch.category_id = compact.optional(compact.category_id, kHasCategory);
    scratch.rating = compact.rating;
    scratch.favorite = compact.has(kFavorite);
    scratch.availability = compact.availability;
    scratch.analysis_flags = compact.analysis_flags;
    if (fields.tags) {
        const auto& tags = tables.tag_sets[compact.tag_set];
        scratch.tag_paths = tags.paths;
        scratch.tag_ids = tags.ids;
    }
    if (fields.category) scratch.category = tables.texts.text(compact.category);
    if (fields.format) scratch.format = tables.texts.text(compact.format);
    if (fields.codec) scratch.codec = tables.texts.text(compact.codec);
    if (fields.folder) {
        scratch.root_name = tables.texts.text(root.name);
        if (root.root_id == 0) {
            scratch.path_key.clear();
        } else if (compact.has(kVerbatimPath)) {
            scratch.path_key = path_key(compact.text.file_name(), root.case_sensitive);
        } else {
            scratch.path_key = folder.key;
            if (!folder.key.empty()) scratch.path_key += '/';
            compact.text.append_file_name(scratch.path_key);
        }
    }
}

// ---------------------------------------------------------------- sorting

/// The value a row is ordered by for one field; `present == false` is the null partition.
struct SortKey {
    bool present = false;
    bool text = false;
    std::int64_t number = 0;
    std::string_view characters;
};

/// The sort key of a number column stored with a presence bit.
SortKey number_key(const CompactRow& row, std::int64_t value, RowBit bit) {
    return row.has(bit) ? SortKey{true, false, value, {}} : SortKey{};
}

/// Mirrors SearchService's SQL sort expressions (s.name_key, e.duration_us, key_class*2+minor...).
SortKey sort_key(const CompactRow& row, const SharedTables& tables, SortField field) {
    switch (field) {
        case SortField::Name:
        case SortField::Relevance:
            return SortKey{true, true, 0, row.text.name_key()};
        case SortField::Added:
            return SortKey{true, false, row.created_at, {}};
        case SortField::Duration:
            return number_key(row, row.duration_us, kHasDuration);
        case SortField::Size:
            return number_key(row, row.byte_size, kHasByteSize);
        case SortField::SampleRate:
            return number_key(row, row.sample_rate, kHasSampleRate);
        case SortField::Bpm:
            return number_key(row, row.bpm_milli, kHasBpm);
        case SortField::Key:
            if (!row.has(kHasKey)) return SortKey{};
            return SortKey{true, false, row.key_pitch_class * 2 + (row.key_mode == KeyMode::Minor ? 1 : 0), {}};
        case SortField::Rating:
            return SortKey{true, false, row.rating, {}};
        case SortField::Format: {
            const auto& format = tables.texts.text(row.format);
            return format.empty() ? SortKey{} : SortKey{true, true, 0, format};
        }
    }
    return SortKey{true, true, 0, row.text.name_key()};
}

/// Three-way comparison of two present keys of the same field (text compares bytewise, like
/// SQLite's BINARY collation on UTF-8).
int compare_present(const SortKey& left, const SortKey& right) {
    if (left.text) {
        const int compared = left.characters.compare(right.characters);
        return compared < 0 ? -1 : compared > 0 ? 1 : 0;
    }
    return left.number < right.number ? -1 : left.number > right.number ? 1 : 0;
}

/// Relevance without ranked text sorts like Name.
SortField effective_sort(SortField field) {
    return field == SortField::Relevance ? SortField::Name : field;
}

/// Slot of a sort field in the per-field order cache.
std::size_t field_index(SortField field) {
    return static_cast<std::size_t>(effective_sort(field));
}

/// Every live slot of an index in ascending order for one field.
struct SortOrder {
    std::vector<std::uint32_t> slots;
    std::size_t non_null = 0;  ///< slots[0, non_null) have a value; the rest are nulls
};

/// A sort key that owns its text, kept while the row it came from is overwritten.
struct StoredKey {
    bool present = false;
    bool text = false;
    std::int64_t number = 0;
    std::string characters;

    /// A copy of `key`.
    static StoredKey of(const SortKey& key) {
        return StoredKey{key.present, key.text, key.number, std::string(key.characters)};
    }
    /// True when `key` orders exactly like this one.
    bool equals(const SortKey& key) const {
        if (present != key.present) return false;
        if (!present) return true;
        return text ? characters == key.characters : number == key.number;
    }
};

/// Where a changed row sat in one cached order before its new values were applied.
struct OldPlacement {
    std::uint32_t slot = 0;
    std::int64_t sample_id = 0;
    std::size_t position = 0;  ///< index in SortOrder::slots
    StoredKey key;             ///< its key for that order's field
};

/// What patching the cached orders needs to know from before a change was applied.
struct OrderPatch {
    bool rebuild = false;                                               ///< too many rows changed: drop every order
    std::array<bool, kSortFieldCount> dropped{};                        ///< per order: rebuild it instead of patching
    std::array<std::vector<OldPlacement>, kSortFieldCount> placements;  ///< per cached order
};

/// A filtered list of slots in ascending order, cached so later pages cost O(log n + page size).
struct FilteredResult {
    std::string key;
    std::uint64_t data_generation = 0;
    std::vector<std::uint32_t> slots;
    std::size_t non_null = 0;
};

/// Membership of sample ids as a bitmap indexed by id (ids are dense SQLite row ids), so testing
/// 100K rows costs a bit lookup each instead of a hash probe.
class SampleIdSet {
public:
    /// The members of `ids` below `id_limit` (larger ids cannot be rows of the index anyway).
    SampleIdSet(const std::vector<std::int64_t>& ids, std::int64_t id_limit) {
        std::int64_t largest = -1;
        for (const auto id : ids)
            if (id < id_limit) largest = std::max(largest, id);
        members_.assign(static_cast<std::size_t>(largest + 1), false);
        for (const auto id : ids)
            if (id >= 0 && id <= largest) members_[static_cast<std::size_t>(id)] = true;
    }
    bool contains(std::int64_t id) const {
        return id >= 0 && static_cast<std::size_t>(id) < members_.size() && members_[static_cast<std::size_t>(id)];
    }

private:
    std::vector<bool> members_;
};

/// A set of sample ids computed by SQLite (full-text terms, collection membership).
struct IdSet {
    std::string key;
    std::uint64_t data_generation = 0;
    std::shared_ptr<const SampleIdSet> ids;
};

/// Navigation data needed to evaluate tag and category subtree scopes (parent links) and merged
/// tag names in `tag:` filters.
struct Taxonomy {
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> tag_children;
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> category_children;
    search::TagAliases aliases;
};

/// `root` and its descendants down to the SQL scope compiler's depth limit (an unknown id yields
/// just itself, which no sample references, exactly like the recursive SQL).
std::unordered_set<std::int64_t> subtree(const std::unordered_map<std::int64_t, std::vector<std::int64_t>>& children,
                                         std::int64_t root) {
    std::unordered_set<std::int64_t> members{root};
    std::vector<std::int64_t> frontier{root};
    for (int depth = 0; depth < kMaximumTaxonomyDepth && !frontier.empty(); ++depth) {
        std::vector<std::int64_t> next;
        for (const auto node : frontier)
            if (const auto found = children.find(node); found != children.end())
                for (const auto child : found->second)
                    if (members.insert(child).second) next.push_back(child);
        frontier = std::move(next);
    }
    return members;
}

/// Reads the tag and category trees and the merged-tag aliases.
Result<Taxonomy> load_taxonomy(catalog::Library& library) {
    auto tags = library.list_tags();
    if (!tags) return std::unexpected(tags.error());
    auto categories = library.list_categories();
    if (!categories) return std::unexpected(categories.error());
    auto aliases = search::load_tag_aliases(library);
    if (!aliases) return std::unexpected(aliases.error());
    Taxonomy taxonomy;
    for (const auto& tag : *tags)
        if (tag.parent_id) taxonomy.tag_children[*tag.parent_id].push_back(tag.id);
    for (const auto& category : *categories)
        if (category.parent_id) taxonomy.category_children[*category.parent_id].push_back(category.id);
    taxonomy.aliases = std::move(*aliases);
    return taxonomy;
}

/// A scope as JSON, for cache keys and cursor fingerprints (same fields as SearchService).
nlohmann::json scope_json(const search::Scope& scope) {
    auto optional_json = [](const auto& value) { return value ? nlohmann::json(*value) : nlohmann::json(); };
    return {{"root", optional_json(scope.root_id)},
            {"folder", optional_json(scope.folder)},
            {"collection", optional_json(scope.collection_id)},
            {"tag", optional_json(scope.tag_id)},
            {"category", optional_json(scope.category_id)},
            {"uncategorized", scope.uncategorized}};
}

// ---------------------------------------------------------------- row storage

/// What applying a batch of fresh rows changed.
struct AppliedRows {
    std::vector<std::uint32_t> changed_slots;
    bool rows_added_or_removed = false;
};

/// Every row of an index with the tables they refer to. Built privately by load() and swapped in.
struct IndexData {
    std::vector<CompactRow> rows;  ///< slots; a removed sample frees its slot for reuse
    std::vector<std::uint32_t> free_slots;
    std::vector<std::uint32_t> slot_by_id;  ///< indexed by sample id; kNoSlot = not live
    std::size_t live_rows = 0;
    std::int64_t id_limit = kMinimumIdLimit;  ///< ids at or above this are not indexed (id_limit_for)
    SharedTables tables;
    Taxonomy taxonomy;

    /// The slot of a live sample.
    std::optional<std::uint32_t> slot_of(std::int64_t sample_id) const {
        if (sample_id < 0 || static_cast<std::size_t>(sample_id) >= slot_by_id.size()) return std::nullopt;
        const auto slot = slot_by_id[static_cast<std::size_t>(sample_id)];
        return slot == kNoSlot ? std::nullopt : std::optional(slot);
    }

    /// Adds a row that is not yet present (its id is below id_limit).
    std::uint32_t insert(CompactRow&& row) {
        std::uint32_t slot = 0;
        if (!free_slots.empty()) {
            slot = free_slots.back();
            free_slots.pop_back();
            rows[slot] = std::move(row);
        } else {
            slot = static_cast<std::uint32_t>(rows.size());
            rows.push_back(std::move(row));
        }
        const auto id = static_cast<std::size_t>(rows[slot].sample_id);
        if (id >= slot_by_id.size())
            slot_by_id.resize(std::min(std::max(id + 1, slot_by_id.size() * 3 / 2), static_cast<std::size_t>(id_limit)),
                              kNoSlot);
        slot_by_id[id] = slot;
        ++live_rows;
        return slot;
    }

    /// Applies `fresh` (the current rows of those `sample_ids` that are live; the others were
    /// removed). RESOURCE_LIMIT for an id the index cannot hold (the caller reloads, which then
    /// fails cleanly and leaves the library to SQLite).
    Result<AppliedRows> apply(std::span<const std::int64_t> sample_ids, std::vector<PreparedRow>&& fresh) {
        std::unordered_map<std::int64_t, PreparedRow*> fresh_by_id;
        for (auto& prepared : fresh)
            fresh_by_id.emplace(prepared.row.sample_id, &prepared);
        AppliedRows applied;
        for (const auto sample_id : sample_ids) {
            const auto found = fresh_by_id.find(sample_id);
            const auto existing = slot_of(sample_id);
            if (found != fresh_by_id.end()) {
                if (sample_id < 0 || sample_id >= id_limit)
                    return fail(ErrorCode::ResourceLimit, "a sample id is too large for the in-memory index");
                auto row = compact(std::move(*found->second), tables);
                if (existing) {
                    rows[*existing] = std::move(row);
                    applied.changed_slots.push_back(*existing);
                } else {
                    applied.changed_slots.push_back(insert(std::move(row)));
                    applied.rows_added_or_removed = true;
                }
                fresh_by_id.erase(found);  // a duplicated id in the change set is applied once
            } else if (existing) {
                // Removed, missing from the live set, or deleted: free the slot.
                rows[*existing] = CompactRow{};
                free_slots.push_back(*existing);
                slot_by_id[static_cast<std::size_t>(sample_id)] = kNoSlot;
                --live_rows;
                applied.rows_added_or_removed = true;
                applied.changed_slots.push_back(*existing);
            }
        }
        return applied;
    }
};

}  // namespace

// ---------------------------------------------------------------- state

/// Everything behind the index's locks.
struct SampleIndex::State {
    /// Serializes synchronize() and the catch-up step of load(); also guards the bookkeeping below.
    mutable std::mutex synchronization_mutex;
    /// True while a load() runs (a second load is refused with DB_BUSY).
    bool loading = false;
    /// While a load runs, acknowledgements never pass the revision it started from, so the outbox
    /// keeps every change the load must catch up with.
    std::optional<std::int64_t> load_floor;
    std::int64_t acknowledged_revision = -1;
    std::chrono::steady_clock::time_point last_acknowledgement{};

    /// Readers share it; load()/synchronize() take it exclusively only to apply finished data.
    mutable std::shared_mutex data_mutex;
    IndexData data;
    std::atomic<std::uint64_t> structure_generation{0};
    std::atomic<std::uint64_t> data_generation{0};
    /// data.id_limit, readable without the lock (SQL id sets are built before any lock is taken).
    std::atomic<std::int64_t> id_limit{kMinimumIdLimit};

    /// Lazily built per-field orders. Readers build under orders_mutex while holding data_mutex
    /// shared; writers patch them while holding data_mutex exclusively.
    mutable std::mutex orders_mutex;
    mutable std::array<std::unique_ptr<SortOrder>, kSortFieldCount> orders;

    mutable std::mutex results_mutex;
    mutable std::deque<std::shared_ptr<const FilteredResult>> results;  ///< most recent first

    mutable std::mutex id_sets_mutex;
    mutable std::deque<IdSet> id_sets;  ///< most recent first

    /// Sort key of the row in `slot` (caller holds data_mutex).
    SortKey key_of(std::uint32_t slot, SortField field) const { return sort_key(data.rows[slot], data.tables, field); }

    /// The layout of every cached order: values ascending, nulls last, ties by ascending sample id.
    bool ascending_before(std::uint32_t left, std::uint32_t right, SortField field) const {
        const SortKey left_key = key_of(left, field);
        const SortKey right_key = key_of(right, field);
        if (left_key.present != right_key.present) return left_key.present;
        if (left_key.present)
            if (const int compared = compare_present(left_key, right_key); compared != 0) return compared < 0;
        return data.rows[left].sample_id < data.rows[right].sample_id;
    }

    /// The ascending order of every live slot for `field` (caller holds data_mutex shared).
    const SortOrder& order(SortField field) const {
        std::lock_guard lock(orders_mutex);
        auto& cached = orders[field_index(field)];
        if (!cached) {
            auto built = std::make_unique<SortOrder>();
            built->slots.reserve(data.live_rows);
            for (std::uint32_t slot = 0; slot < data.rows.size(); ++slot)
                if (data.rows[slot].live) built->slots.push_back(slot);
            const SortField sort = effective_sort(field);
            std::ranges::sort(built->slots, [&](std::uint32_t left, std::uint32_t right) {
                return ascending_before(left, right, sort);
            });
            built->non_null = count_present(built->slots, sort);
            cached = std::move(built);
        }
        return *cached;
    }

    /// Length of the leading run of slots whose key for `field` is present.
    std::size_t count_present(const std::vector<std::uint32_t>& slots, SortField field) const {
        const auto first_null =
            std::ranges::partition_point(slots, [&](std::uint32_t slot) { return key_of(slot, field).present; });
        return static_cast<std::size_t>(first_null - slots.begin());
    }

    /// Records where the rows of `sample_ids` sit in every cached order, before their new values
    /// overwrite them (caller holds data_mutex exclusively). patch_orders() needs these positions:
    /// once a row is overwritten, a binary search can no longer find its old place.
    OrderPatch plan_order_patch(std::span<const std::int64_t> sample_ids) const {
        OrderPatch patch;
        std::lock_guard lock(orders_mutex);
        if (sample_ids.size() > kMaximumPatchedRows) {
            patch.rebuild = true;
            return patch;
        }
        for (std::size_t index = 0; index < orders.size(); ++index) {
            const auto& order = orders[index];
            if (!order) continue;
            const auto field = static_cast<SortField>(index);
            auto& placements = patch.placements[index];
            for (const auto sample_id : sample_ids) {
                const auto slot = data.slot_of(sample_id);
                if (!slot) continue;
                const auto found = std::ranges::lower_bound(
                    order->slots, *slot,
                    [&](std::uint32_t left, std::uint32_t right) { return ascending_before(left, right, field); });
                if (found == order->slots.end() || *found != *slot) {
                    patch.dropped[index] = true;  // the order does not hold the row: rebuild it
                    break;
                }
                placements.push_back(OldPlacement{*slot, sample_id,
                                                  static_cast<std::size_t>(found - order->slots.begin()),
                                                  StoredKey::of(key_of(*slot, field))});
            }
        }
        return patch;
    }

    /// Moves the changed rows to their new places in every cached order (caller holds data_mutex
    /// exclusively; `patch` was planned before the rows changed). A row whose key for an order did
    /// not change keeps its place in that order; the others are erased in one pass and inserted in
    /// one pass at positions found by binary search, so a single rating edit costs two block moves
    /// in the rating order and nothing in the others, instead of a merge of every order.
    void patch_orders(const OrderPatch& patch, std::vector<std::uint32_t> changed) {
        std::ranges::sort(changed);
        changed.erase(std::ranges::unique(changed).begin(), changed.end());
        std::lock_guard lock(orders_mutex);
        for (std::size_t index = 0; index < orders.size(); ++index) {
            auto& order = orders[index];
            if (!order) continue;
            if (patch.rebuild || patch.dropped[index] || changed.size() > kMaximumPatchedRows) {
                order.reset();  // rebuilt on next use
                continue;
            }
            const auto field = static_cast<SortField>(index);
            std::vector<std::size_t> removed_positions;
            std::vector<std::uint32_t> unmoved;
            for (const auto& placement : patch.placements[index]) {
                const auto& row = data.rows[placement.slot];
                if (row.live && row.sample_id == placement.sample_id &&
                    placement.key.equals(key_of(placement.slot, field)))
                    unmoved.push_back(placement.slot);
                else
                    removed_positions.push_back(placement.position);
            }
            std::ranges::sort(unmoved);
            std::vector<std::uint32_t> inserted;
            for (const auto slot : changed)
                if (data.rows[slot].live && !std::ranges::binary_search(unmoved, slot)) inserted.push_back(slot);
            if (removed_positions.empty() && inserted.empty()) continue;
            erase_positions(order->slots, std::move(removed_positions));
            insert_sorted(order->slots, std::move(inserted), field);
            order->non_null = count_present(order->slots, field);
        }
    }

    /// Removes the entries at `positions` from `slots` in one pass.
    static void erase_positions(std::vector<std::uint32_t>& slots, std::vector<std::size_t> positions) {
        if (positions.empty()) return;
        std::ranges::sort(positions);
        positions.erase(std::ranges::unique(positions).begin(), positions.end());
        auto write = slots.begin() + static_cast<std::ptrdiff_t>(positions.front());
        for (std::size_t at = 0; at < positions.size(); ++at) {
            const auto from = slots.begin() + static_cast<std::ptrdiff_t>(positions[at]) + 1;
            const auto to = at + 1 < positions.size() ? slots.begin() + static_cast<std::ptrdiff_t>(positions[at + 1])
                                                      : slots.end();
            write = std::move(from, to, write);
        }
        slots.erase(write, slots.end());
    }

    /// Inserts `inserted` into the sorted `slots` in one backward pass. Every entry of `slots` must
    /// be ordered by the current row values (no entry may be stale), so the binary searches agree.
    void insert_sorted(std::vector<std::uint32_t>& slots, std::vector<std::uint32_t> inserted, SortField field) const {
        if (inserted.empty()) return;
        auto before = [&](std::uint32_t left, std::uint32_t right) { return ascending_before(left, right, field); };
        std::ranges::sort(inserted, before);
        std::vector<std::size_t> destinations;
        destinations.reserve(inserted.size());
        for (const auto slot : inserted)
            destinations.push_back(
                static_cast<std::size_t>(std::ranges::lower_bound(slots, slot, before) - slots.begin()));
        const std::size_t old_size = slots.size();
        slots.resize(old_size + inserted.size());
        auto source_end = slots.begin() + static_cast<std::ptrdiff_t>(old_size);
        auto destination_end = slots.end();
        for (std::size_t at = inserted.size(); at-- > 0;) {
            const auto source_begin = slots.begin() + static_cast<std::ptrdiff_t>(destinations[at]);
            destination_end = std::move_backward(source_begin, source_end, destination_end);
            *--destination_end = inserted[at];
            source_end = source_begin;
        }
    }

    /// Drops every derived cache (caller holds data_mutex exclusively).
    void reset_caches() {
        {
            std::lock_guard lock(orders_mutex);
            for (auto& order : orders)
                order.reset();
        }
        clear_query_caches();
    }

    /// Drops the filtered results and SQL id sets.
    void clear_query_caches() const {
        {
            std::lock_guard lock(results_mutex);
            results.clear();
        }
        std::lock_guard lock(id_sets_mutex);
        id_sets.clear();
    }

    /// Stores the outbox cursor at most every kAcknowledgeInterval, never past a running load's
    /// starting revision (synchronization_mutex held).
    std::optional<Error> acknowledge(catalog::Library& library, std::int64_t revision) {
        if (load_floor) revision = std::min(revision, *load_floor);
        if (revision <= acknowledged_revision) return std::nullopt;
        const auto now = std::chrono::steady_clock::now();
        if (acknowledged_revision >= 0 && now - last_acknowledgement < kAcknowledgeInterval) return std::nullopt;
        last_acknowledgement = now;
        if (auto stored = library.acknowledge_changes(SampleIndex::kConsumerName, revision); !stored)
            return stored.error();
        acknowledged_revision = revision;
        return std::nullopt;
    }

    /// A cached filtered list of `key` computed at `generation` (moved to the front), or null.
    std::shared_ptr<const FilteredResult> cached_result(const std::string& key, std::uint64_t generation) const {
        std::lock_guard lock(results_mutex);
        for (auto entry = results.begin(); entry != results.end(); ++entry) {
            if ((*entry)->key != key || (*entry)->data_generation != generation) continue;
            auto found = *entry;
            results.erase(entry);
            results.push_front(found);
            return found;
        }
        return nullptr;
    }

    /// Caches a filtered list, dropping the least recently used beyond kCachedResults.
    void remember_result(std::shared_ptr<const FilteredResult> result) const {
        std::lock_guard lock(results_mutex);
        results.push_front(std::move(result));
        while (results.size() > SampleIndex::kCachedResults)
            results.pop_back();
    }

    /// A cached SQL id set of `key` computed at `generation`, or null.
    std::shared_ptr<const SampleIdSet> cached_ids(const std::string& key, std::uint64_t generation) const {
        std::lock_guard lock(id_sets_mutex);
        for (const auto& entry : id_sets)
            if (entry.key == key && entry.data_generation == generation) return entry.ids;
        return nullptr;
    }

    /// Caches a SQL id set, dropping the oldest beyond kCachedIdSets.
    void remember_ids(IdSet entry) const {
        std::lock_guard lock(id_sets_mutex);
        id_sets.push_front(std::move(entry));
        while (id_sets.size() > kCachedIdSets)
            id_sets.pop_back();
    }

    /// Swaps in finished data (caller holds synchronization_mutex) and returns the previous data.
    /// The caller destroys it after releasing synchronization_mutex too: freeing a million rows
    /// takes a while, and neither readers nor synchronize() should wait for it.
    [[nodiscard]] IndexData publish(IndexData&& fresh) {
        std::unique_lock exclusive(data_mutex);
        IndexData previous = std::exchange(data, std::move(fresh));
        reset_caches();
        id_limit.store(data.id_limit, std::memory_order_release);
        structure_generation.fetch_add(1, std::memory_order_acq_rel);
        data_generation.fetch_add(1, std::memory_order_acq_rel);
        return previous;
    }
};

// ---------------------------------------------------------------- SampleIndex

SampleIndex::SampleIndex(uuids::uuid library) : library_(library), state_(std::make_unique<State>()) {}

SampleIndex::~SampleIndex() = default;

Result<void> SampleIndex::load(catalog::Library& library, std::stop_token stop, std::size_t row_limit) {
    auto& state = *state_;
    std::int64_t start_revision = 0;
    {
        std::lock_guard synchronization(state.synchronization_mutex);
        if (state.loading) return fail(ErrorCode::DbBusy, "the sample index is already loading", true);
        // Read under the mutex together with setting the floor: no acknowledgement can pass it.
        auto revision = library.catalog_revision();
        if (!revision) return std::unexpected(revision.error());
        start_revision = *revision;
        state.loading = true;
        state.load_floor = start_revision;
    }
    /// Ends the loading state on every return.
    struct LoadingScope {
        State& state;
        ~LoadingScope() {
            std::lock_guard synchronization(state.synchronization_mutex);
            state.loading = false;
            state.load_floor.reset();
        }
    } loading_scope{state};
    const auto limit_error = [row_limit] {
        return fail(ErrorCode::ResourceLimit,
                    std::format("the library has more than {} samples, the in-memory index limit", row_limit));
    };
    const auto id_error = [] {
        return fail(ErrorCode::ResourceLimit, "a sample id is too large for the in-memory index");
    };

    // A library over the limit is refused before anything is streamed; otherwise the rows are
    // reserved once (a growing vector would briefly hold 1.5 times the rows while it moves them).
    auto live_samples = library.sample_count();
    if (!live_samples) return std::unexpected(live_samples.error());
    if (*live_samples < 0 || static_cast<std::size_t>(*live_samples) > row_limit) return limit_error();
    IndexData fresh;
    fresh.id_limit = id_limit_for(row_limit);
    fresh.rows.reserve(static_cast<std::size_t>(*live_samples));

    // Stream without any index lock: synchronize() keeps serving edits on the previous state.
    bool too_many = false;
    bool id_out_of_range = false;
    // The stream stops as soon as `stop` fires and ends its read snapshot, so closing a library
    // during its first load releases it promptly.
    auto streamed = search::for_each_row(library, stop, [&](search::SampleRow&& row) {
        if (too_many || id_out_of_range) return;
        if (fresh.live_rows >= row_limit) {
            too_many = true;
            return;
        }
        if (row.sample_id < 0 || row.sample_id >= fresh.id_limit) {
            id_out_of_range = true;
            return;
        }
        if (fresh.slot_of(row.sample_id)) return;  // the stream yields each sample once; ignore repeats
        auto name_key = fold_key(row.display_name);
        fresh.insert(compact(PreparedRow{std::move(row), std::move(name_key)}, fresh.tables));
    });
    if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "loading the sample index was cancelled");
    if (!streamed) return std::unexpected(streamed.error());
    if (too_many) return limit_error();
    if (id_out_of_range) return id_error();
    auto taxonomy = load_taxonomy(library);
    if (!taxonomy) return std::unexpected(taxonomy.error());
    fresh.taxonomy = std::move(*taxonomy);

    // Catch up with what was committed while streaming, then swap the result in. The previous
    // data is declared before the lock so it is freed after the mutex is released.
    IndexData previous;
    std::lock_guard synchronization(state.synchronization_mutex);
    auto changes = library.changes_since(start_revision, kChangesPerSynchronization);
    if (!changes) return std::unexpected(changes.error());
    std::int64_t published_revision = start_revision;  // with a full reload, the next synchronize asks again
    if (!changes->full_reload) {
        if (!changes->sample_ids.empty()) {
            auto rows = search::load_rows(library, changes->sample_ids);
            if (!rows) return std::unexpected(rows.error());
            auto applied = fresh.apply(changes->sample_ids, prepare_rows(std::move(*rows)));
            if (!applied) return std::unexpected(applied.error());
        }
        if (changes->structure_changed) {
            auto reread = load_taxonomy(library);
            if (!reread) return std::unexpected(reread.error());
            fresh.taxonomy = std::move(*reread);
        }
        if (fresh.live_rows > row_limit) return limit_error();
        published_revision = changes->revision;
    }
    previous = state.publish(std::move(fresh));
    revision_.store(published_revision, std::memory_order_release);
    ready_.store(true, std::memory_order_release);
    return {};
}

Result<IndexChange> SampleIndex::synchronize(catalog::Library& library) {
    auto& state = *state_;
    std::lock_guard synchronization(state.synchronization_mutex);
    if (!ready()) return fail(ErrorCode::IndexUnavailable, "the sample index is still loading", true);
    auto changes = library.changes_since(revision(), kChangesPerSynchronization);
    if (!changes) return std::unexpected(changes.error());
    IndexChange change;
    if (changes->full_reload) {
        change.reload_required = true;
        return change;
    }

    // SQL first, without any index lock held, so readers are never blocked behind a query.
    std::vector<PreparedRow> loaded;
    if (!changes->sample_ids.empty()) {
        auto rows = search::load_rows(library, changes->sample_ids);
        if (!rows) return std::unexpected(rows.error());
        loaded = prepare_rows(std::move(*rows));
    }
    std::optional<Taxonomy> taxonomy;
    if (changes->structure_changed) {
        auto read = load_taxonomy(library);
        if (!read) return std::unexpected(read.error());
        taxonomy = std::move(*read);
    }

    if (!changes->sample_ids.empty() || taxonomy) {
        std::unique_lock exclusive(state.data_mutex);
        // Where the changed rows sit in the cached orders, read before their new values land.
        const auto order_patch = state.plan_order_patch(changes->sample_ids);
        auto applied = state.data.apply(changes->sample_ids, std::move(loaded));
        if (!applied) {
            // The rows applied so far stay; a reload rebuilds everything consistently.
            state.reset_caches();
            state.data_generation.fetch_add(1, std::memory_order_acq_rel);
            state.structure_generation.fetch_add(1, std::memory_order_acq_rel);
            change.reload_required = true;
            return change;
        }
        change.rows_added_or_removed = applied->rows_added_or_removed;
        if (taxonomy) {
            state.data.taxonomy = std::move(*taxonomy);
            change.structure_changed = true;
        }
        state.patch_orders(order_patch, std::move(applied->changed_slots));
        state.clear_query_caches();
        state.data_generation.fetch_add(1, std::memory_order_acq_rel);
        if (change.rows_added_or_removed || change.structure_changed)
            state.structure_generation.fetch_add(1, std::memory_order_acq_rel);
        change.sample_ids = std::move(changes->sample_ids);
    }
    revision_.store(changes->revision, std::memory_order_release);
    change.acknowledge_error = state.acknowledge(library, changes->revision);
    return change;
}

void SampleIndex::clear() {
    auto& state = *state_;
    IndexData previous;  // freed after both locks are released
    std::lock_guard synchronization(state.synchronization_mutex);
    {
        // Readiness changes under the exclusive lock, so a search that holds the shared lock
        // either sees the rows or sees "not ready".
        std::unique_lock exclusive(state.data_mutex);
        ready_.store(false, std::memory_order_release);
    }
    previous = state.publish(IndexData{});
}

IndexGenerations SampleIndex::generations() const noexcept {
    return IndexGenerations{state_->structure_generation.load(std::memory_order_acquire),
                            state_->data_generation.load(std::memory_order_acquire)};
}

std::size_t SampleIndex::size() const {
    std::shared_lock lock(state_->data_mutex);
    return state_->data.live_rows;
}

std::optional<search::SampleRow> SampleIndex::row(std::int64_t sample_id) const {
    std::shared_lock lock(state_->data_mutex);
    const auto& data = state_->data;
    const auto slot = data.slot_of(sample_id);
    if (!slot) return std::nullopt;
    return expand(data.rows[*slot], data.tables, library_);
}

std::vector<search::SampleRow> SampleIndex::rows(std::span<const std::int64_t> sample_ids) const {
    std::vector<search::SampleRow> found_rows;
    found_rows.reserve(sample_ids.size());
    std::shared_lock lock(state_->data_mutex);
    const auto& data = state_->data;
    for (const auto sample_id : sample_ids)
        if (const auto slot = data.slot_of(sample_id))
            found_rows.push_back(expand(data.rows[*slot], data.tables, library_));
    return found_rows;
}

std::vector<IndexedFolder> SampleIndex::folders(std::int64_t root_id) const {
    std::map<std::string, IndexedFolder, std::less<>> by_path;
    auto folder_at = [&](std::string_view path) -> IndexedFolder& {
        auto found = by_path.find(path);
        if (found == by_path.end()) found = by_path.emplace(std::string(path), IndexedFolder{std::string(path)}).first;
        return found->second;
    };
    if (root_id != 0) {
        std::shared_lock lock(state_->data_mutex);
        const auto& data = state_->data;
        // Count per folder entry in one pass over the rows, then walk each folder's ancestors once.
        std::vector<std::int64_t> direct(data.tables.folders.size(), 0);
        for (const auto& row : data.rows)
            if (row.live) ++direct[row.folder];
        for (std::size_t number = 0; number < direct.size(); ++number) {
            const auto& folder = data.tables.folders[number];
            if (direct[number] == 0 || data.tables.roots[folder.root].root_id != root_id) continue;
            std::string_view current = folder.path;
            folder_at(current).direct_samples += direct[number];
            while (true) {
                folder_at(current).total_samples += direct[number];
                if (current.empty()) break;
                const auto slash = current.rfind('/');
                current = slash == std::string_view::npos ? std::string_view() : current.substr(0, slash);
            }
        }
    }
    std::vector<IndexedFolder> folders;
    folders.reserve(by_path.size());
    for (auto& [path, folder] : by_path)
        folders.push_back(std::move(folder));
    return folders;
}

void SampleIndex::drop_query_caches() const {
    state_->clear_query_caches();
}

bool SampleIndex::serves(const search::SearchRequest& request) {
    if (request.sort != SortField::Relevance) return true;
    auto query = search::parse_query(request.text);
    return !query || serves(*query, request.sort);
}

bool SampleIndex::serves(const search::Query& query, search::SortField sort) {
    return sort != SortField::Relevance || search::positive_text_terms(query).empty();
}

// ---------------------------------------------------------------- federated evaluation

namespace detail {

/// Evaluates search and count requests over several indexes. A friend of SampleIndex.
class IndexedSearch {
public:
    /// One page of a request (see search_indexes()).
    static Result<search::SearchPage> search(std::span<const IndexedLibrary> libraries,
                                             const search::SearchRequest& request, const search::Query& query);
    /// The exact count of a request (see count_indexes()).
    static Result<search::CountResult> count(std::span<const IndexedLibrary> libraries,
                                             const search::SearchRequest& request, const search::Query& query);

private:
    /// Which SQL id set a request needs from each library.
    enum class IdSetKind { None, TextAndScope, Collection };

    /// A parsed request, shared by every library.
    struct Plan {
        const search::Query* query = nullptr;  ///< the caller's parsed request text
        search::SearchRequest request;
        SortField sort = SortField::Name;  ///< effective (Relevance without ranked terms = Name)
        IdSetKind id_set = IdSetKind::None;
        MatchedFields matched_fields;  ///< row fields the query's predicates read
        std::string id_set_key;        ///< cache key of the SQL id set
        std::string result_key;        ///< cache key of the filtered list (direction-independent)
        std::string fingerprint;       ///< cursor fingerprint (includes the direction)
    };

    /// One library's part of a request.
    struct Participant {
        const IndexedLibrary* source = nullptr;
        const SampleIndex::State* state = nullptr;
        search::LibraryCoverage coverage;
        std::shared_ptr<const SampleIdSet> ids;
        std::uint64_t ids_generation = 0;  ///< data generation the id set was computed for
        std::shared_ptr<const FilteredResult> result;
        std::uint64_t structure_generation = 0;
    };

    /// Where a library's stream resumes: the sort key and id of the last row it returned.
    struct CursorKey {
        bool present = false;
        bool text = false;
        std::int64_t number = 0;
        std::string characters;
        std::int64_t sample_id = 0;
    };

    /// A decoded cursor: the resume key of every library that returned rows before.
    using ResumeKeys = std::map<uuids::uuid, CursorKey>;

    /// Scope predicates resolved against one index (same semantics as SQL's compile_scope). Root
    /// and folder scopes become one flag per folder entry, tag scopes one flag per tag set.
    struct ScopeFilter {
        std::vector<bool> folder_accepted;   ///< empty = no root/folder scope
        std::vector<bool> tag_set_accepted;  ///< empty = no tag scope
        std::optional<std::unordered_set<std::int64_t>> category_ids;
        bool uncategorized = false;

        /// True when `row` lies inside the scope.
        bool accepts(const CompactRow& row) const {
            if (!folder_accepted.empty() && !folder_accepted[row.folder]) return false;
            if (!tag_set_accepted.empty() && !tag_set_accepted[row.tag_set]) return false;
            if (category_ids && (!row.has(kHasCategory) || !category_ids->contains(row.category_id))) return false;
            if (uncategorized && row.has(kHasCategory)) return false;
            return true;
        }
    };

    static Result<Plan> make_plan(const search::SearchRequest& request, const search::Query& query);
    /// Phase 1: SQL id sets, run before any index lock is taken.
    static Result<std::vector<Participant>> prepare(std::span<const IndexedLibrary> libraries, const Plan& plan);
    /// Takes every participant's data lock shared, in a canonical order (state address) so
    /// concurrent searches can never deadlock with a waiting writer; INDEX_UNAVAILABLE when an
    /// index is not ready.
    static Result<std::vector<std::shared_lock<std::shared_mutex>>> lock_all(
        const std::vector<Participant>& participants);
    /// Phase 2 (data_mutex held shared): the filtered ascending list of one participant.
    static std::shared_ptr<const FilteredResult> filtered(const Participant& participant, const Plan& plan);
    static ScopeFilter scope_filter(const IndexData& data, const search::Scope& scope);
    static std::string encode_cursor(const Plan& plan, const std::vector<Participant>& participants,
                                     const ResumeKeys& keys);
    /// Resume keys stored in a cursor; STALE_CURSOR when it belongs to another query or a structure
    /// generation moved.
    static Result<ResumeKeys> decode_cursor(const Plan& plan, const std::vector<Participant>& participants);
    /// The slot at position `position` of a result read in the request's direction.
    static std::uint32_t slot_at(const FilteredResult& result, std::size_t position, bool descending);
    /// Where a row lies relative to a resume key in the request's direction: negative before it,
    /// zero at it, positive after it.
    static int compare_to_key(const SortKey& key, std::int64_t sample_id, const CursorKey& resume, bool descending);
    /// The resume key of a row.
    static CursorKey key_of(const SortKey& key, std::int64_t sample_id);
};

Result<IndexedSearch::Plan> IndexedSearch::make_plan(const search::SearchRequest& request, const search::Query& query) {
    if (request.scope.folder && !request.scope.root_id)
        return fail(ErrorCode::InvalidArgument, "a folder scope needs the root it belongs to");
    Plan plan;
    plan.query = &query;
    plan.request = request;
    plan.sort = effective_sort(request.sort);
    if (!query.empty()) collect_fields(*query.root, plan.matched_fields);
    const bool row_evaluable = query.empty() || search::is_row_evaluable(query);
    if (!row_evaluable)
        plan.id_set = IdSetKind::TextAndScope;
    else if (request.scope.collection_id)
        plan.id_set = IdSetKind::Collection;
    const auto scope = scope_json(request.scope);
    if (plan.id_set == IdSetKind::TextAndScope)
        plan.id_set_key = json_fields::dump_json({{"text", request.text}, {"scope", scope}});
    else if (plan.id_set == IdSetKind::Collection)
        plan.id_set_key = json_fields::dump_json({{"collection", *request.scope.collection_id}});
    plan.result_key =
        json_fields::dump_json({{"text", request.text}, {"scope", scope}, {"sort", to_string(plan.sort)}});
    plan.fingerprint = picosha2::hash256_hex_string(json_fields::dump_json({{"text", request.text},
                                                                            {"scope", scope},
                                                                            {"sort", to_string(request.sort)},
                                                                            {"descending", request.descending}}));
    return plan;
}

Result<std::vector<IndexedSearch::Participant>> IndexedSearch::prepare(std::span<const IndexedLibrary> libraries,
                                                                       const Plan& plan) {
    std::vector<Participant> participants;
    participants.reserve(libraries.size());
    for (const auto& source : libraries) {
        Participant participant;
        participant.source = &source;
        participant.state = source.index->state_.get();
        participant.coverage = search::LibraryCoverage{source.index->library(), source.name, true, std::nullopt};
        if (plan.id_set != IdSetKind::None) {
            // The generation is read before the SQL runs: the ids then describe the catalog at least
            // as new as this generation, and are cached (and trusted) for this generation only.
            participant.ids_generation = participant.state->data_generation.load(std::memory_order_acquire);
            participant.ids = participant.state->cached_ids(plan.id_set_key, participant.ids_generation);
            if (!participant.ids) {
                search::SearchRequest sql_request;
                if (plan.id_set == IdSetKind::TextAndScope) {
                    sql_request.text = plan.request.text;
                    sql_request.scope = plan.request.scope;
                } else {
                    sql_request.scope.collection_id = plan.request.scope.collection_id;
                }
                // An id set never holds more ids than the library has live samples; the bound
                // (with room for rows added since the index last synchronized) keeps a damaged or
                // runaway query from growing it further. RESOURCE_LIMIT is this library's failure.
                const std::size_t maximum_ids = source.index->size() + SampleIndex::kChangesPerSynchronization;
                auto ids = search::matching_ids(*source.library, sql_request, maximum_ids);
                if (!ids) {
                    // A query error is the caller's; any other failure excludes this library only.
                    if (ids.error().code == ErrorCode::InvalidArgument || ids.error().code == ErrorCode::ParseError ||
                        ids.error().code == ErrorCode::StaleCursor)
                        return std::unexpected(ids.error());
                    participant.coverage.complete = false;
                    participant.coverage.error = ids.error();
                } else {
                    participant.ids = std::make_shared<const SampleIdSet>(
                        *ids, participant.state->id_limit.load(std::memory_order_acquire));
                    participant.state->remember_ids(
                        IdSet{plan.id_set_key, participant.ids_generation, participant.ids});
                }
            }
        }
        participants.push_back(std::move(participant));
    }
    return participants;
}

Result<std::vector<std::shared_lock<std::shared_mutex>>> IndexedSearch::lock_all(
    const std::vector<Participant>& participants) {
    std::vector<const SampleIndex::State*> lock_order;
    for (const auto& participant : participants)
        lock_order.push_back(participant.state);
    std::ranges::sort(lock_order);
    lock_order.erase(std::ranges::unique(lock_order).begin(), lock_order.end());
    std::vector<std::shared_lock<std::shared_mutex>> locks;
    locks.reserve(lock_order.size());
    for (const auto* state : lock_order)
        locks.emplace_back(state->data_mutex);
    // clear() changes readiness under the exclusive lock, so this check holds while locked.
    for (const auto& participant : participants)
        if (!participant.source->index->ready())
            return fail(ErrorCode::IndexUnavailable, "the sample index is not loaded", true);
    return locks;
}

IndexedSearch::ScopeFilter IndexedSearch::scope_filter(const IndexData& data, const search::Scope& scope) {
    ScopeFilter filter;
    const auto& tables = data.tables;
    if (scope.root_id) {
        // Like compile_scope: the folder's path key without trailing separators, plus "/".
        auto folder_prefix = [&](bool case_sensitive) {
            if (!scope.folder) return std::string();
            std::string key = path_key(*scope.folder, case_sensitive);
            while (key.ends_with('/'))
                key.pop_back();
            return key.empty() ? key : key + '/';
        };
        const std::string folded_prefix = folder_prefix(false);
        const std::string exact_prefix = folder_prefix(true);
        filter.folder_accepted.assign(tables.folders.size(), false);
        for (std::size_t number = 0; number < tables.folders.size(); ++number) {
            const auto& folder = tables.folders[number];
            const auto& root = tables.roots[folder.root];
            if (root.root_id == 0 || root.root_id != *scope.root_id) continue;
            // A location key is "<folder key>/<file key>": it starts with a prefix that ends in '/'
            // exactly when "<folder key>/" does, since file names hold no '/'.
            const auto& prefix = root.case_sensitive ? exact_prefix : folded_prefix;
            filter.folder_accepted[number] = prefix.empty() || (folder.key + '/').starts_with(prefix);
        }
    }
    if (scope.tag_id) {
        const auto tags = subtree(data.taxonomy.tag_children, *scope.tag_id);
        filter.tag_set_accepted.assign(tables.tag_sets.size(), false);
        for (std::size_t number = 0; number < tables.tag_sets.size(); ++number)
            filter.tag_set_accepted[number] =
                std::ranges::any_of(tables.tag_sets[number].ids, [&](std::int64_t tag) { return tags.contains(tag); });
    }
    if (scope.category_id) filter.category_ids = subtree(data.taxonomy.category_children, *scope.category_id);
    filter.uncategorized = scope.uncategorized;
    return filter;
}

std::shared_ptr<const FilteredResult> IndexedSearch::filtered(const Participant& participant, const Plan& plan) {
    const auto& state = *participant.state;
    const auto& data = state.data;
    const auto generation = state.data_generation.load(std::memory_order_acquire);
    if (auto cached = state.cached_result(plan.result_key, generation)) return cached;

    const auto& order = state.order(plan.sort);
    auto result = std::make_shared<FilteredResult>();
    result->key = plan.result_key;
    result->data_generation = generation;
    const auto* ids = participant.ids.get();
    if (plan.id_set == IdSetKind::TextAndScope) {
        // SQLite already applied the text terms and the whole scope.
        for (std::size_t position = 0; position < order.slots.size(); ++position) {
            const auto slot = order.slots[position];
            if (!ids->contains(data.rows[slot].sample_id)) continue;
            result->slots.push_back(slot);
            if (position < order.non_null) ++result->non_null;
        }
    } else {
        const auto scope = scope_filter(data, plan.request.scope);
        const std::span<const search::TagAliases> aliases(&data.taxonomy.aliases, 1);
        const search::RowMatcher matcher(*plan.query, aliases);  // one per call: it caches normalizations
        search::SampleRow scratch;
        scratch.library = participant.source->index->library();
        for (std::size_t position = 0; position < order.slots.size(); ++position) {
            const auto slot = order.slots[position];
            const auto& row = data.rows[slot];
            if (!scope.accepts(row)) continue;
            if (plan.id_set == IdSetKind::Collection && !ids->contains(row.sample_id)) continue;
            if (!plan.query->empty()) {
                fill_for_matching(scratch, row, data.tables, plan.matched_fields);
                if (!matcher(scratch)) continue;
            }
            result->slots.push_back(slot);
            if (position < order.non_null) ++result->non_null;
        }
    }
    // A list built from an id set of an older generation answers this search, but must not be cached
    // as describing the current generation.
    if (plan.id_set == IdSetKind::None || participant.ids_generation == generation) state.remember_result(result);
    return result;
}

std::uint32_t IndexedSearch::slot_at(const FilteredResult& result, std::size_t position, bool descending) {
    if (!descending) return result.slots[position];
    // Descending keeps nulls last: the value run reversed, then the null run reversed.
    const std::size_t present = result.non_null;
    const std::size_t total = result.slots.size();
    return position < present ? result.slots[present - 1 - position] : result.slots[total - 1 - (position - present)];
}

int IndexedSearch::compare_to_key(const SortKey& key, std::int64_t sample_id, const CursorKey& resume,
                                  bool descending) {
    if (key.present != resume.present) return key.present ? -1 : 1;  // nulls last in both directions
    if (key.present) {
        int compared = 0;
        if (resume.text) {
            const int raw = key.characters.compare(resume.characters);
            compared = raw < 0 ? -1 : raw > 0 ? 1 : 0;
        } else {
            compared = key.number < resume.number ? -1 : key.number > resume.number ? 1 : 0;
        }
        if (compared != 0) return descending ? -compared : compared;
    }
    if (sample_id == resume.sample_id) return 0;
    const bool before = descending ? sample_id > resume.sample_id : sample_id < resume.sample_id;
    return before ? -1 : 1;
}

IndexedSearch::CursorKey IndexedSearch::key_of(const SortKey& key, std::int64_t sample_id) {
    return CursorKey{key.present, key.text, key.number, std::string(key.characters), sample_id};
}

std::string IndexedSearch::encode_cursor(const Plan& plan, const std::vector<Participant>& participants,
                                         const ResumeKeys& keys) {
    // Only libraries that answered this page are recorded: one that was missing and answers on a
    // later page has no position to resume from, so the cursor must go stale rather than restart
    // that library at its first row.
    nlohmann::json generations = nlohmann::json::object();
    for (const auto& participant : participants)
        if (participant.coverage.complete)
            generations[uuids::to_string(participant.coverage.library)] = participant.structure_generation;
    nlohmann::json positions = nlohmann::json::object();
    for (const auto& [library, key] : keys) {
        nlohmann::json value;         // null for the null partition
        if (key.present && key.text)  // as bytes, so any text round-trips exactly
            value = std::vector<std::uint8_t>(key.characters.begin(), key.characters.end());
        else if (key.present)
            value = key.number;
        positions[uuids::to_string(library)] = nlohmann::json::array({key.present, key.text, value, key.sample_id});
    }
    return json_fields::dump_json(
        {{"v", kIndexCursorVersion}, {"q", plan.fingerprint}, {"g", generations}, {"p", positions}});
}

Result<IndexedSearch::ResumeKeys> IndexedSearch::decode_cursor(const Plan& plan,
                                                               const std::vector<Participant>& participants) {
    ResumeKeys keys;
    if (!plan.request.cursor) return keys;
    auto stale = [] { return fail(ErrorCode::StaleCursor, "the result set changed; refresh the view"); };
    const auto document = nlohmann::json::parse(plan.request.cursor->token, nullptr, false);
    if (json_fields::integer_field(document, "v") != kIndexCursorVersion ||
        json_fields::string_field(document, "q") != plan.fingerprint)
        return stale();
    const auto* generations = json_fields::object_field(document, "g");
    const auto* positions = json_fields::object_field(document, "p");
    const auto answering = static_cast<std::size_t>(std::ranges::count_if(
        participants, [](const Participant& participant) { return participant.coverage.complete; }));
    if (generations == nullptr || positions == nullptr || generations->size() != answering) return stale();
    for (const auto& participant : participants) {
        if (!participant.coverage.complete) continue;  // excluded from this page, as from the earlier ones
        const auto library = uuids::to_string(participant.coverage.library);
        const auto stored = json_fields::integer_field(*generations, library);
        if (!stored || static_cast<std::uint64_t>(*stored) != participant.structure_generation) return stale();
        const auto* position = json_fields::array_field(*positions, library);
        if (position == nullptr) continue;  // this library returned no row yet
        if (position->size() != 4 || !(*position)[0].is_boolean() || !(*position)[1].is_boolean() ||
            !(*position)[3].is_number_integer())
            return stale();
        CursorKey key;
        key.present = (*position)[0].get<bool>();
        key.text = (*position)[1].get<bool>();
        key.sample_id = (*position)[3].get<std::int64_t>();
        const auto& value = (*position)[2];
        if (key.present && key.text) {
            if (!value.is_array()) return stale();
            for (const auto& byte : value) {
                if (!byte.is_number_unsigned() || byte.get<std::uint64_t>() > 0xFF) return stale();
                key.characters.push_back(static_cast<char>(byte.get<std::uint8_t>()));
            }
        } else if (key.present) {
            if (!value.is_number_integer()) return stale();
            key.number = value.get<std::int64_t>();
        }
        keys.emplace(participant.coverage.library, std::move(key));
    }
    if (keys.size() != positions->size()) return stale();  // a position of a library not taking part
    return keys;
}

Result<search::SearchPage> IndexedSearch::search(std::span<const IndexedLibrary> libraries,
                                                 const search::SearchRequest& request, const search::Query& query) {
    auto plan = make_plan(request, query);
    if (!plan) return std::unexpected(plan.error());
    auto participants = prepare(libraries, *plan);
    if (!participants) return std::unexpected(participants.error());
    const std::size_t page_size = std::clamp<std::size_t>(request.page_size, 1, search::SearchService::kMaxPageSize);
    auto locks = lock_all(*participants);
    if (!locks) return std::unexpected(locks.error());

    for (auto& participant : *participants) {
        participant.structure_generation = participant.state->structure_generation.load(std::memory_order_acquire);
        if (participant.coverage.complete) participant.result = filtered(participant, *plan);
    }
    auto resume = decode_cursor(*plan, *participants);
    if (!resume) return std::unexpected(resume.error());

    const bool descending = request.descending;
    const SortField sort = plan->sort;
    /// One library's read position in the merge.
    struct Stream {
        const Participant* participant = nullptr;
        std::size_t position = 0;
        bool exhausted() const { return position >= participant->result->slots.size(); }
        /// The row at `at` of this library's result in the request's direction.
        const CompactRow& row_at(std::size_t at, bool descending_order) const {
            return participant->state->data.rows[slot_at(*participant->result, at, descending_order)];
        }
        const CompactRow& head(bool descending_order) const { return row_at(position, descending_order); }
    };
    auto key_at = [&](const Stream& stream, std::size_t at) {
        return sort_key(stream.row_at(at, descending), stream.participant->state->data.tables, sort);
    };

    std::vector<Stream> streams;
    for (const auto& participant : *participants) {
        if (!participant.result || participant.result->slots.empty()) continue;
        Stream stream{&participant, 0};
        if (const auto resume_key = resume->find(participant.coverage.library); resume_key != resume->end()) {
            // Keyset: the first row strictly after the last one this library returned, found by
            // binary search in the current order (values may have changed since).
            const auto positions = std::views::iota(std::size_t{0}, participant.result->slots.size());
            stream.position = *std::ranges::partition_point(positions, [&](std::size_t at) {
                return compare_to_key(key_at(stream, at), stream.row_at(at, descending).sample_id, resume_key->second,
                                      descending) <= 0;
            });
        }
        streams.push_back(stream);
    }

    // Global order: values (in direction), nulls last, then library UUID, then sample id.
    auto comes_before = [&](const Stream& left, const Stream& right) {
        const SortKey left_key = key_at(left, left.position);
        const SortKey right_key = key_at(right, right.position);
        if (left_key.present != right_key.present) return left_key.present;
        if (left_key.present)
            if (const int compared = compare_present(left_key, right_key); compared != 0)
                return descending ? compared > 0 : compared < 0;
        const auto& left_library = left.participant->coverage.library;
        const auto& right_library = right.participant->coverage.library;
        if (left_library != right_library) return left_library < right_library;
        const auto left_id = left.head(descending).sample_id;
        const auto right_id = right.head(descending).sample_id;
        return descending ? left_id > right_id : left_id < right_id;
    };

    search::SearchPage page;
    ResumeKeys next_keys = std::move(*resume);
    while (page.rows.size() < page_size) {
        Stream* best = nullptr;
        for (auto& stream : streams)
            if (!stream.exhausted() && (best == nullptr || comes_before(stream, *best))) best = &stream;
        if (best == nullptr) break;
        const auto& row = best->head(descending);
        const auto& library = best->participant->coverage.library;
        page.rows.push_back(expand(row, best->participant->state->data.tables, library));
        next_keys[library] = key_of(key_at(*best, best->position), row.sample_id);
        ++best->position;
    }
    const bool more = std::ranges::any_of(streams, [](const Stream& stream) { return !stream.exhausted(); });
    if (more) page.next = search::Cursor{encode_cursor(*plan, *participants, next_keys)};
    for (auto& participant : *participants)
        page.coverage.push_back(std::move(participant.coverage));
    return page;
}

Result<search::CountResult> IndexedSearch::count(std::span<const IndexedLibrary> libraries,
                                                 const search::SearchRequest& request, const search::Query& query) {
    auto plan = make_plan(request, query);
    if (!plan) return std::unexpected(plan.error());
    auto participants = prepare(libraries, *plan);
    if (!participants) return std::unexpected(participants.error());
    auto locks = lock_all(*participants);
    if (!locks) return std::unexpected(locks.error());
    search::CountResult result;
    for (auto& participant : *participants) {
        if (participant.coverage.complete)
            result.total += static_cast<std::int64_t>(filtered(participant, *plan)->slots.size());
        result.coverage.push_back(std::move(participant.coverage));
    }
    return result;
}

}  // namespace detail

Result<search::SearchPage> search_indexes(std::span<const IndexedLibrary> libraries,
                                          const search::SearchRequest& request) {
    auto query = search::parse_query(request.text);
    if (!query) return std::unexpected(query.error());
    return detail::IndexedSearch::search(libraries, request, *query);
}

Result<search::SearchPage> search_indexes(std::span<const IndexedLibrary> libraries,
                                          const search::SearchRequest& request, const search::Query& query) {
    return detail::IndexedSearch::search(libraries, request, query);
}

Result<search::CountResult> count_indexes(std::span<const IndexedLibrary> libraries,
                                          const search::SearchRequest& request) {
    auto query = search::parse_query(request.text);
    if (!query) return std::unexpected(query.error());
    return detail::IndexedSearch::count(libraries, request, *query);
}

Result<search::CountResult> count_indexes(std::span<const IndexedLibrary> libraries,
                                          const search::SearchRequest& request, const search::Query& query) {
    return detail::IndexedSearch::count(libraries, request, query);
}

}  // namespace mso::app
