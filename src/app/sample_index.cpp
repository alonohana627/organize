#include "mso/app/sample_index.hpp"

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "mso/app/json_fields.hpp"

namespace mso::app {

namespace {

using search::SortField;

/// Cursor version of index cursors; SearchService cursors use 1, so each rejects the other's
/// tokens as STALE_CURSOR instead of misreading them.
constexpr std::int64_t kIndexCursorVersion = 101;
/// Number of SortField values (Relevance shares Name's order).
constexpr std::size_t kSortFieldCount = 10;
/// Minimum spacing of outbox acknowledgements (each one is a small catalog write).
constexpr auto kAcknowledgeInterval = std::chrono::seconds(5);
/// Above this many changed rows a cached sort order is dropped and rebuilt on demand instead of
/// being patched in place.
constexpr std::size_t kMaximumPatchedRows = 2048;
/// SQL id sets kept per index (full-text terms, collection membership).
constexpr std::size_t kCachedIdSets = 4;
/// Depth limit of tag subtrees, as in the SQL scope compiler.
constexpr int kMaximumTaxonomyDepth = 16;

/// One indexed sample with its precomputed sort and scope keys.
struct IndexedRow {
    search::SampleRow row;
    std::string name_key;      ///< fold_key(display_name): the catalog's samples.name_key
    std::string location_key;  ///< path_key of the primary location: the catalog's locations.path_key
    bool live = false;         ///< false for a free slot
};

/// Wraps a loaded row with its precomputed keys.
IndexedRow make_indexed(search::SampleRow&& row, const uuids::uuid& library) {
    IndexedRow indexed;
    indexed.name_key = fold_key(row.display_name);
    indexed.location_key = path_key(row.relative_path, row.root_case_sensitive);
    indexed.row = std::move(row);
    indexed.row.library = library;
    indexed.row.relevance = 0;
    indexed.live = true;
    return indexed;
}

/// The value a row is ordered by for one field; `present == false` is the null partition.
struct SortKey {
    bool present = false;
    bool text = false;
    std::int64_t number = 0;
    std::string_view characters;
};

/// The sort key of an optional integer column.
SortKey number_key(const std::optional<std::int64_t>& value) {
    return value ? SortKey{true, false, *value, {}} : SortKey{};
}

/// Mirrors SearchService's SQL sort expressions (s.name_key, e.duration_us, key_class*2+minor...).
SortKey sort_key(const IndexedRow& indexed, SortField field) {
    const auto& row = indexed.row;
    switch (field) {
        case SortField::Name:
        case SortField::Relevance:
            return SortKey{true, true, 0, indexed.name_key};
        case SortField::Added:
            return SortKey{true, false, row.created_at, {}};
        case SortField::Duration:
            return number_key(row.duration_us);
        case SortField::Size:
            return number_key(row.byte_size);
        case SortField::SampleRate:
            return number_key(row.sample_rate);
        case SortField::Bpm:
            return number_key(row.bpm_milli);
        case SortField::Key:
            if (!row.key) return SortKey{};
            return SortKey{true, false, row.key->pitch_class * 2 + (row.key->mode == KeyMode::Minor ? 1 : 0), {}};
        case SortField::Rating:
            return SortKey{true, false, row.rating, {}};
        case SortField::Format:
            return row.format.empty() ? SortKey{} : SortKey{true, true, 0, row.format};
    }
    return SortKey{true, true, 0, indexed.name_key};
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

/// The layout of every cached order: values ascending, nulls last, ties by ascending sample id.
bool ascending_before(const IndexedRow& left, const IndexedRow& right, SortField field) {
    const SortKey left_key = sort_key(left, field);
    const SortKey right_key = sort_key(right, field);
    if (left_key.present != right_key.present) return left_key.present;
    if (left_key.present)
        if (const int compared = compare_present(left_key, right_key); compared != 0) return compared < 0;
    return left.row.sample_id < right.row.sample_id;
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

/// A filtered list of slots in ascending order, cached so later pages cost O(page size).
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
    explicit SampleIdSet(const std::vector<std::int64_t>& ids) {
        std::int64_t largest = -1;
        for (const auto id : ids)
            largest = std::max(largest, id);
        members_.assign(static_cast<std::size_t>(largest + 1), false);
        for (const auto id : ids)
            if (id >= 0) members_[static_cast<std::size_t>(id)] = true;
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

/// Navigation data needed to evaluate tag and category subtree scopes (parent links).
struct Taxonomy {
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> tag_children;
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> category_children;
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

/// Reads the tag and category trees.
Result<Taxonomy> load_taxonomy(catalog::Library& library) {
    auto tags = library.list_tags();
    if (!tags) return std::unexpected(tags.error());
    auto categories = library.list_categories();
    if (!categories) return std::unexpected(categories.error());
    Taxonomy taxonomy;
    for (const auto& tag : *tags)
        if (tag.parent_id) taxonomy.tag_children[*tag.parent_id].push_back(tag.id);
    for (const auto& category : *categories)
        if (category.parent_id) taxonomy.category_children[*category.parent_id].push_back(category.id);
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

}  // namespace

// ---------------------------------------------------------------- state

/// Everything behind the index's locks.
struct SampleIndex::State {
    /// Serializes load() and synchronize(); also guards the acknowledgement bookkeeping.
    std::mutex synchronization_mutex;
    std::int64_t acknowledged_revision = -1;
    std::chrono::steady_clock::time_point last_acknowledgement{};

    /// Readers share it; load()/synchronize() take it exclusively only to swap in finished data.
    mutable std::shared_mutex data_mutex;
    std::vector<IndexedRow> rows;  ///< slots; a removed sample frees its slot for reuse
    std::vector<std::uint32_t> free_slots;
    std::unordered_map<std::int64_t, std::uint32_t> slot_by_id;
    std::size_t live_rows = 0;
    Taxonomy taxonomy;
    std::atomic<std::uint64_t> structure_generation{0};
    std::atomic<std::uint64_t> data_generation{0};

    /// Lazily built per-field orders. Readers build under orders_mutex while holding data_mutex
    /// shared; writers patch them while holding data_mutex exclusively.
    mutable std::mutex orders_mutex;
    mutable std::array<std::unique_ptr<SortOrder>, kSortFieldCount> orders;

    mutable std::mutex results_mutex;
    mutable std::deque<std::shared_ptr<const FilteredResult>> results;  ///< most recent first

    mutable std::mutex id_sets_mutex;
    mutable std::deque<IdSet> id_sets;  ///< most recent first

    /// The ascending order of every live slot for `field` (caller holds data_mutex shared).
    const SortOrder& order(SortField field) const {
        std::lock_guard lock(orders_mutex);
        auto& cached = orders[field_index(field)];
        if (!cached) {
            auto built = std::make_unique<SortOrder>();
            built->slots.reserve(live_rows);
            for (std::uint32_t slot = 0; slot < rows.size(); ++slot)
                if (rows[slot].live) built->slots.push_back(slot);
            const SortField sort = effective_sort(field);
            std::ranges::sort(built->slots, [&](std::uint32_t left, std::uint32_t right) {
                return ascending_before(rows[left], rows[right], sort);
            });
            built->non_null = count_present(built->slots, sort);
            cached = std::move(built);
        }
        return *cached;
    }

    /// Length of the leading run of slots whose key for `field` is present.
    std::size_t count_present(const std::vector<std::uint32_t>& slots, SortField field) const {
        const auto first_null = std::ranges::partition_point(
            slots, [&](std::uint32_t slot) { return sort_key(rows[slot], field).present; });
        return static_cast<std::size_t>(first_null - slots.begin());
    }

    /// Re-sorts the changed slots into every cached order (caller holds data_mutex exclusively).
    void patch_orders(std::vector<std::uint32_t> changed) {
        std::ranges::sort(changed);
        changed.erase(std::ranges::unique(changed).begin(), changed.end());
        std::lock_guard lock(orders_mutex);
        if (changed.empty()) return;
        std::vector<bool> is_changed(rows.size(), false);
        for (const auto slot : changed)
            is_changed[slot] = true;
        for (std::size_t index = 0; index < orders.size(); ++index) {
            auto& order = orders[index];
            if (!order) continue;
            if (changed.size() > kMaximumPatchedRows) {
                order.reset();  // rebuilt on next use
                continue;
            }
            const auto field = static_cast<SortField>(index);
            auto kept = order->slots;
            std::erase_if(kept, [&](std::uint32_t slot) { return slot < is_changed.size() && is_changed[slot]; });
            std::vector<std::uint32_t> inserted;
            for (const auto slot : changed)
                if (rows[slot].live) inserted.push_back(slot);
            auto before = [&](std::uint32_t left, std::uint32_t right) {
                return ascending_before(rows[left], rows[right], field);
            };
            std::ranges::sort(inserted, before);
            std::vector<std::uint32_t> merged;
            merged.reserve(kept.size() + inserted.size());
            std::ranges::merge(kept, inserted, std::back_inserter(merged), before);
            order->non_null = count_present(merged, field);
            order->slots = std::move(merged);
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

    void clear_query_caches() {
        {
            std::lock_guard lock(results_mutex);
            results.clear();
        }
        std::lock_guard lock(id_sets_mutex);
        id_sets.clear();
    }

    /// Stores the outbox cursor at most every kAcknowledgeInterval (synchronization_mutex held).
    std::optional<Error> acknowledge(catalog::Library& library, std::int64_t revision) {
        if (revision <= acknowledged_revision) return std::nullopt;
        const auto now = std::chrono::steady_clock::now();
        if (acknowledged_revision >= 0 && now - last_acknowledgement < kAcknowledgeInterval) return std::nullopt;
        last_acknowledgement = now;
        if (auto stored = library.acknowledge_changes(SampleIndex::kConsumerName, revision); !stored)
            return stored.error();
        acknowledged_revision = revision;
        return std::nullopt;
    }

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

    void remember_result(std::shared_ptr<const FilteredResult> result) const {
        std::lock_guard lock(results_mutex);
        results.push_front(std::move(result));
        while (results.size() > SampleIndex::kCachedResults)
            results.pop_back();
    }

    std::shared_ptr<const SampleIdSet> cached_ids(const std::string& key, std::uint64_t generation) const {
        std::lock_guard lock(id_sets_mutex);
        for (const auto& entry : id_sets)
            if (entry.key == key && entry.data_generation == generation) return entry.ids;
        return nullptr;
    }

    void remember_ids(IdSet entry) const {
        std::lock_guard lock(id_sets_mutex);
        id_sets.push_front(std::move(entry));
        while (id_sets.size() > kCachedIdSets)
            id_sets.pop_back();
    }
};

// ---------------------------------------------------------------- SampleIndex

SampleIndex::SampleIndex(uuids::uuid library) : library_(library), state_(std::make_unique<State>()) {}

SampleIndex::~SampleIndex() = default;

Result<void> SampleIndex::load(catalog::Library& library, std::stop_token stop) {
    std::lock_guard synchronization(state_->synchronization_mutex);
    // The revision is read before streaming: changes committed while rows stream in are applied
    // again by the next synchronize(), which is harmless because rows are reloaded by id.
    auto revision = library.catalog_revision();
    if (!revision) return std::unexpected(revision.error());
    const std::int64_t start_revision = *revision;
    std::vector<IndexedRow> rows;
    std::unordered_map<std::int64_t, std::uint32_t> slot_by_id;
    bool too_many = false;
    auto streamed = search::for_each_row(library, [&](search::SampleRow&& row) {
        if (stop.stop_requested() || too_many) return;
        if (rows.size() >= std::numeric_limits<std::uint32_t>::max()) {
            too_many = true;
            return;
        }
        if (!slot_by_id.emplace(row.sample_id, static_cast<std::uint32_t>(rows.size())).second) return;
        rows.push_back(make_indexed(std::move(row), library_));
    });
    if (!streamed) return std::unexpected(streamed.error());
    if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "loading the sample index was cancelled");
    if (too_many) return fail(ErrorCode::ResourceLimit, "the library has too many samples for the in-memory index");
    auto taxonomy = load_taxonomy(library);
    if (!taxonomy) return std::unexpected(taxonomy.error());
    {
        std::unique_lock exclusive(state_->data_mutex);
        state_->live_rows = rows.size();
        state_->rows = std::move(rows);
        state_->slot_by_id = std::move(slot_by_id);
        state_->free_slots.clear();
        state_->taxonomy = std::move(*taxonomy);
        state_->reset_caches();
        state_->structure_generation.fetch_add(1, std::memory_order_acq_rel);
        state_->data_generation.fetch_add(1, std::memory_order_acq_rel);
    }
    revision_.store(start_revision, std::memory_order_release);
    ready_.store(true, std::memory_order_release);
    return {};
}

Result<IndexChange> SampleIndex::synchronize(catalog::Library& library) {
    std::lock_guard synchronization(state_->synchronization_mutex);
    if (!ready()) return fail(ErrorCode::IndexUnavailable, "the sample index is still loading", true);
    auto changes = library.changes_since(revision(), kChangesPerSynchronization);
    if (!changes) return std::unexpected(changes.error());
    IndexChange change;
    if (changes->full_reload) {
        change.reload_required = true;
        return change;
    }

    // SQL first, without any index lock held, so readers are never blocked behind a query.
    std::vector<search::SampleRow> loaded;
    if (!changes->sample_ids.empty()) {
        auto rows = search::load_rows(library, changes->sample_ids);
        if (!rows) return std::unexpected(rows.error());
        loaded = std::move(*rows);
    }
    std::optional<Taxonomy> taxonomy;
    if (changes->structure_changed) {
        auto read = load_taxonomy(library);
        if (!read) return std::unexpected(read.error());
        taxonomy = std::move(*read);
    }

    if (!changes->sample_ids.empty() || taxonomy) {
        std::unordered_map<std::int64_t, search::SampleRow*> loaded_by_id;
        for (auto& row : loaded)
            loaded_by_id.emplace(row.sample_id, &row);
        std::unique_lock exclusive(state_->data_mutex);
        auto& state = *state_;
        std::vector<std::uint32_t> changed;
        for (const auto sample_id : changes->sample_ids) {
            const auto fresh = loaded_by_id.find(sample_id);
            const auto existing = state.slot_by_id.find(sample_id);
            if (fresh != loaded_by_id.end()) {
                if (existing != state.slot_by_id.end()) {
                    state.rows[existing->second] = make_indexed(std::move(*fresh->second), library_);
                    changed.push_back(existing->second);
                } else {
                    std::uint32_t slot = 0;
                    if (!state.free_slots.empty()) {
                        slot = state.free_slots.back();
                        state.free_slots.pop_back();
                    } else {
                        slot = static_cast<std::uint32_t>(state.rows.size());
                        state.rows.emplace_back();
                    }
                    state.rows[slot] = make_indexed(std::move(*fresh->second), library_);
                    state.slot_by_id.emplace(sample_id, slot);
                    ++state.live_rows;
                    change.rows_added_or_removed = true;
                    changed.push_back(slot);
                }
                loaded_by_id.erase(fresh);  // a duplicated id in the change set is applied once
            } else if (existing != state.slot_by_id.end()) {
                // Removed, missing from the live set, or deleted: free the slot.
                const auto slot = existing->second;
                state.rows[slot] = IndexedRow{};
                state.free_slots.push_back(slot);
                state.slot_by_id.erase(existing);
                --state.live_rows;
                change.rows_added_or_removed = true;
                changed.push_back(slot);
            }
        }
        if (taxonomy) {
            state.taxonomy = std::move(*taxonomy);
            change.structure_changed = true;
        }
        state.patch_orders(std::move(changed));
        state.clear_query_caches();
        state.data_generation.fetch_add(1, std::memory_order_acq_rel);
        if (change.rows_added_or_removed || change.structure_changed)
            state.structure_generation.fetch_add(1, std::memory_order_acq_rel);
        change.sample_ids = std::move(changes->sample_ids);
    }
    revision_.store(changes->revision, std::memory_order_release);
    change.acknowledge_error = state_->acknowledge(library, changes->revision);
    return change;
}

IndexGenerations SampleIndex::generations() const noexcept {
    return IndexGenerations{state_->structure_generation.load(std::memory_order_acquire),
                            state_->data_generation.load(std::memory_order_acquire)};
}

std::size_t SampleIndex::size() const {
    std::shared_lock lock(state_->data_mutex);
    return state_->live_rows;
}

std::optional<search::SampleRow> SampleIndex::row(std::int64_t sample_id) const {
    std::shared_lock lock(state_->data_mutex);
    const auto found = state_->slot_by_id.find(sample_id);
    if (found == state_->slot_by_id.end()) return std::nullopt;
    return state_->rows[found->second].row;
}

std::vector<search::SampleRow> SampleIndex::rows(std::span<const std::int64_t> sample_ids) const {
    std::vector<search::SampleRow> found_rows;
    found_rows.reserve(sample_ids.size());
    std::shared_lock lock(state_->data_mutex);
    for (const auto sample_id : sample_ids)
        if (const auto found = state_->slot_by_id.find(sample_id); found != state_->slot_by_id.end())
            found_rows.push_back(state_->rows[found->second].row);
    return found_rows;
}

std::vector<IndexedFolder> SampleIndex::folders(std::int64_t root_id) const {
    std::map<std::string, IndexedFolder, std::less<>> by_path;
    auto folder_at = [&](std::string_view path) -> IndexedFolder& {
        auto found = by_path.find(path);
        if (found == by_path.end()) found = by_path.emplace(std::string(path), IndexedFolder{std::string(path)}).first;
        return found->second;
    };
    {
        std::shared_lock lock(state_->data_mutex);
        for (const auto& indexed : state_->rows) {
            if (!indexed.live || indexed.row.root_id != root_id) continue;
            std::string_view current = indexed.row.folder_path;
            ++folder_at(current).direct_samples;
            while (true) {
                ++folder_at(current).total_samples;
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
    return !query || search::positive_text_terms(*query).empty();
}

// ---------------------------------------------------------------- federated evaluation

namespace detail {

/// Evaluates search and count requests over several indexes. A friend of SampleIndex.
class IndexedSearch {
public:
    static Result<search::SearchPage> search(std::span<const IndexedLibrary> libraries,
                                             const search::SearchRequest& request);
    static Result<search::CountResult> count(std::span<const IndexedLibrary> libraries,
                                             const search::SearchRequest& request);

private:
    /// Which SQL id set a request needs from each library.
    enum class IdSetKind { None, TextAndScope, Collection };

    /// A parsed request, shared by every library.
    struct Plan {
        search::Query query;
        search::SearchRequest request;
        SortField sort = SortField::Name;  ///< effective (Relevance without ranked terms = Name)
        IdSetKind id_set = IdSetKind::None;
        std::string id_set_key;   ///< cache key of the SQL id set
        std::string result_key;   ///< cache key of the filtered list (direction-independent)
        std::string fingerprint;  ///< cursor fingerprint (includes the direction)
    };

    /// One library's part of a request.
    struct Participant {
        const IndexedLibrary* source = nullptr;
        const SampleIndex::State* state = nullptr;
        search::LibraryCoverage coverage;
        std::shared_ptr<const SampleIdSet> ids;
        std::shared_ptr<const FilteredResult> result;
        std::uint64_t structure_generation = 0;
    };

    /// Scope predicates resolved against one index's taxonomy (same semantics as SQL's compile_scope).
    struct ScopeFilter {
        std::optional<std::int64_t> root_id;
        std::string folder_prefix_folded;  ///< "<folder key>/" for case-insensitive roots; "" = whole root
        std::string folder_prefix_exact;   ///< the same for case-sensitive roots
        std::optional<std::unordered_set<std::int64_t>> tag_ids;
        std::optional<std::unordered_set<std::int64_t>> category_ids;
        bool uncategorized = false;

        bool accepts(const IndexedRow& indexed) const {
            const auto& row = indexed.row;
            if (root_id) {
                if (row.root_id != *root_id) return false;
                const auto& prefix = row.root_case_sensitive ? folder_prefix_exact : folder_prefix_folded;
                if (!prefix.empty() && !indexed.location_key.starts_with(prefix)) return false;
            }
            if (tag_ids && std::ranges::none_of(row.tag_ids, [&](std::int64_t tag) { return tag_ids->contains(tag); }))
                return false;
            if (category_ids && (!row.category_id || !category_ids->contains(*row.category_id))) return false;
            if (uncategorized && row.category_id) return false;
            return true;
        }
    };

    static Result<Plan> make_plan(const search::SearchRequest& request);
    /// Phase 1: SQL id sets, run before any index lock is taken.
    static Result<std::vector<Participant>> prepare(std::span<const IndexedLibrary> libraries, const Plan& plan);
    /// Phase 2 (data_mutex held shared): the filtered ascending list of one participant.
    static std::shared_ptr<const FilteredResult> filtered(const Participant& participant, const Plan& plan);
    static ScopeFilter scope_filter(const SampleIndex::State& state, const search::Scope& scope);
    static std::string encode_cursor(const Plan& plan, const std::vector<Participant>& participants,
                                     std::size_t offset);
    /// Offset stored in a cursor; STALE_CURSOR when it belongs to another query or a structure
    /// generation moved.
    static Result<std::size_t> decode_cursor(const Plan& plan, const std::vector<Participant>& participants);
    /// The slot at position `position` of a result read in the request's direction.
    static std::uint32_t slot_at(const FilteredResult& result, std::size_t position, bool descending);
};

Result<IndexedSearch::Plan> IndexedSearch::make_plan(const search::SearchRequest& request) {
    auto query = search::parse_query(request.text);
    if (!query) return std::unexpected(query.error());
    if (request.scope.folder && !request.scope.root_id)
        return fail(ErrorCode::InvalidArgument, "a folder scope needs the root it belongs to");
    Plan plan;
    plan.query = std::move(*query);
    plan.request = request;
    plan.sort = effective_sort(request.sort);
    const bool row_evaluable = plan.query.empty() || search::is_row_evaluable(plan.query);
    if (!row_evaluable)
        plan.id_set = IdSetKind::TextAndScope;
    else if (request.scope.collection_id)
        plan.id_set = IdSetKind::Collection;
    const auto scope = scope_json(request.scope);
    if (plan.id_set == IdSetKind::TextAndScope)
        plan.id_set_key = dump_json({{"text", request.text}, {"scope", scope}});
    else if (plan.id_set == IdSetKind::Collection)
        plan.id_set_key = dump_json({{"collection", *request.scope.collection_id}});
    plan.result_key = dump_json({{"text", request.text}, {"scope", scope}, {"sort", to_string(plan.sort)}});
    plan.fingerprint = picosha2::hash256_hex_string(dump_json({{"text", request.text},
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
            const auto generation = participant.state->data_generation.load(std::memory_order_acquire);
            participant.ids = participant.state->cached_ids(plan.id_set_key, generation);
            if (!participant.ids) {
                search::SearchRequest sql_request;
                if (plan.id_set == IdSetKind::TextAndScope) {
                    sql_request.text = plan.request.text;
                    sql_request.scope = plan.request.scope;
                } else {
                    sql_request.scope.collection_id = plan.request.scope.collection_id;
                }
                auto ids = search::matching_ids(*source.library, sql_request);
                if (!ids) {
                    // A query error is the caller's; any other failure excludes this library only.
                    if (ids.error().code == ErrorCode::InvalidArgument || ids.error().code == ErrorCode::ParseError ||
                        ids.error().code == ErrorCode::StaleCursor)
                        return std::unexpected(ids.error());
                    participant.coverage.complete = false;
                    participant.coverage.error = ids.error();
                } else {
                    participant.ids = std::make_shared<const SampleIdSet>(*ids);
                    participant.state->remember_ids(IdSet{plan.id_set_key, generation, participant.ids});
                }
            }
        }
        participants.push_back(std::move(participant));
    }
    return participants;
}

IndexedSearch::ScopeFilter IndexedSearch::scope_filter(const SampleIndex::State& state, const search::Scope& scope) {
    ScopeFilter filter;
    filter.root_id = scope.root_id;
    if (scope.root_id && scope.folder) {
        // Like compile_scope: the folder's path key without trailing separators, plus "/".
        auto folder_prefix = [&](bool case_sensitive) {
            std::string key = path_key(*scope.folder, case_sensitive);
            while (key.ends_with('/'))
                key.pop_back();
            return key.empty() ? key : key + '/';
        };
        filter.folder_prefix_folded = folder_prefix(false);
        filter.folder_prefix_exact = folder_prefix(true);
    }
    if (scope.tag_id) filter.tag_ids = subtree(state.taxonomy.tag_children, *scope.tag_id);
    if (scope.category_id) filter.category_ids = subtree(state.taxonomy.category_children, *scope.category_id);
    filter.uncategorized = scope.uncategorized;
    return filter;
}

std::shared_ptr<const FilteredResult> IndexedSearch::filtered(const Participant& participant, const Plan& plan) {
    const auto& state = *participant.state;
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
            if (!ids->contains(state.rows[slot].row.sample_id)) continue;
            result->slots.push_back(slot);
            if (position < order.non_null) ++result->non_null;
        }
    } else {
        const auto scope = scope_filter(state, plan.request.scope);
        const search::RowMatcher matcher(plan.query);  // one per call: it caches normalizations
        for (std::size_t position = 0; position < order.slots.size(); ++position) {
            const auto slot = order.slots[position];
            const auto& indexed = state.rows[slot];
            if (!scope.accepts(indexed)) continue;
            if (plan.id_set == IdSetKind::Collection && !ids->contains(indexed.row.sample_id)) continue;
            if (!plan.query.empty() && !matcher(indexed.row)) continue;
            result->slots.push_back(slot);
            if (position < order.non_null) ++result->non_null;
        }
    }
    state.remember_result(result);
    return result;
}

std::uint32_t IndexedSearch::slot_at(const FilteredResult& result, std::size_t position, bool descending) {
    if (!descending) return result.slots[position];
    // Descending keeps nulls last: the value run reversed, then the null run reversed.
    const std::size_t present = result.non_null;
    const std::size_t total = result.slots.size();
    return position < present ? result.slots[present - 1 - position] : result.slots[total - 1 - (position - present)];
}

std::string IndexedSearch::encode_cursor(const Plan& plan, const std::vector<Participant>& participants,
                                         std::size_t offset) {
    nlohmann::json generations = nlohmann::json::object();
    for (const auto& participant : participants)
        generations[uuids::to_string(participant.coverage.library)] = participant.structure_generation;
    return dump_json({{"v", kIndexCursorVersion}, {"q", plan.fingerprint}, {"g", generations}, {"o", offset}});
}

Result<std::size_t> IndexedSearch::decode_cursor(const Plan& plan, const std::vector<Participant>& participants) {
    if (!plan.request.cursor) return 0;
    auto stale = [] { return fail(ErrorCode::StaleCursor, "the result set changed; refresh the view"); };
    const auto document = nlohmann::json::parse(plan.request.cursor->token, nullptr, false);
    if (integer_field(document, "v") != kIndexCursorVersion || string_field(document, "q") != plan.fingerprint)
        return stale();
    const auto* generations = object_field(document, "g");
    const auto offset = integer_field(document, "o");
    if (generations == nullptr || !offset || *offset < 0 || generations->size() != participants.size()) return stale();
    for (const auto& participant : participants) {
        const auto stored = integer_field(*generations, uuids::to_string(participant.coverage.library));
        if (!stored || static_cast<std::uint64_t>(*stored) != participant.structure_generation) return stale();
    }
    return static_cast<std::size_t>(*offset);
}

Result<search::SearchPage> IndexedSearch::search(std::span<const IndexedLibrary> libraries,
                                                 const search::SearchRequest& request) {
    auto plan = make_plan(request);
    if (!plan) return std::unexpected(plan.error());
    auto participants = prepare(libraries, *plan);
    if (!participants) return std::unexpected(participants.error());
    const std::size_t page_size = std::clamp<std::size_t>(request.page_size, 1, search::SearchService::kMaxPageSize);

    // Lock in a canonical order (state address) so concurrent searches can never deadlock with a
    // waiting writer.
    std::vector<const SampleIndex::State*> lock_order;
    for (const auto& participant : *participants)
        lock_order.push_back(participant.state);
    std::ranges::sort(lock_order);
    lock_order.erase(std::ranges::unique(lock_order).begin(), lock_order.end());
    std::vector<std::shared_lock<std::shared_mutex>> locks;
    locks.reserve(lock_order.size());
    for (const auto* state : lock_order)
        locks.emplace_back(state->data_mutex);

    for (auto& participant : *participants) {
        participant.structure_generation = participant.state->structure_generation.load(std::memory_order_acquire);
        if (participant.coverage.complete) participant.result = filtered(participant, *plan);
    }
    auto offset = decode_cursor(*plan, *participants);
    if (!offset) return std::unexpected(offset.error());

    /// One library's read position in the merge.
    struct Stream {
        const Participant* participant = nullptr;
        std::size_t position = 0;
        bool exhausted() const { return position >= participant->result->slots.size(); }
    };
    std::vector<Stream> streams;
    for (const auto& participant : *participants)
        if (participant.result && !participant.result->slots.empty()) streams.push_back(Stream{&participant, 0});

    const bool descending = request.descending;
    const SortField sort = plan->sort;
    auto head = [&](const Stream& stream) -> const IndexedRow& {
        return stream.participant->state->rows[slot_at(*stream.participant->result, stream.position, descending)];
    };
    // Global order: values (in direction), nulls last, then library UUID, then sample id.
    auto comes_before = [&](const Stream& left, const Stream& right) {
        const auto& left_row = head(left);
        const auto& right_row = head(right);
        const SortKey left_key = sort_key(left_row, sort);
        const SortKey right_key = sort_key(right_row, sort);
        if (left_key.present != right_key.present) return left_key.present;
        if (left_key.present)
            if (const int compared = compare_present(left_key, right_key); compared != 0)
                return descending ? compared > 0 : compared < 0;
        if (left_row.row.library != right_row.row.library) return left_row.row.library < right_row.row.library;
        return descending ? left_row.row.sample_id > right_row.row.sample_id
                          : left_row.row.sample_id < right_row.row.sample_id;
    };

    search::SearchPage page;
    std::size_t consumed = 0;
    if (streams.size() == 1) {
        auto& stream = streams.front();
        stream.position = std::min(*offset, stream.participant->result->slots.size());
        consumed = stream.position;
        while (!stream.exhausted() && page.rows.size() < page_size) {
            page.rows.push_back(head(stream).row);
            ++stream.position;
        }
    } else {
        while (page.rows.size() < page_size) {
            Stream* best = nullptr;
            for (auto& stream : streams)
                if (!stream.exhausted() && (best == nullptr || comes_before(stream, *best))) best = &stream;
            if (best == nullptr) break;
            if (consumed >= *offset) page.rows.push_back(head(*best).row);
            ++consumed;
            ++best->position;
        }
    }
    const bool more = std::ranges::any_of(streams, [](const Stream& stream) { return !stream.exhausted(); });
    const std::size_t next_offset = streams.size() == 1 ? streams.front().position : consumed;
    if (more) page.next = search::Cursor{encode_cursor(*plan, *participants, next_offset)};
    for (auto& participant : *participants)
        page.coverage.push_back(std::move(participant.coverage));
    return page;
}

Result<search::CountResult> IndexedSearch::count(std::span<const IndexedLibrary> libraries,
                                                 const search::SearchRequest& request) {
    auto plan = make_plan(request);
    if (!plan) return std::unexpected(plan.error());
    auto participants = prepare(libraries, *plan);
    if (!participants) return std::unexpected(participants.error());
    search::CountResult result;
    for (auto& participant : *participants) {
        if (participant.coverage.complete) {
            std::shared_lock lock(participant.state->data_mutex);
            result.total += static_cast<std::int64_t>(filtered(participant, *plan)->slots.size());
        }
        result.coverage.push_back(std::move(participant.coverage));
    }
    return result;
}

}  // namespace detail

Result<search::SearchPage> search_indexes(std::span<const IndexedLibrary> libraries,
                                          const search::SearchRequest& request) {
    return detail::IndexedSearch::search(libraries, request);
}

Result<search::CountResult> count_indexes(std::span<const IndexedLibrary> libraries,
                                          const search::SearchRequest& request) {
    return detail::IndexedSearch::count(libraries, request);
}

}  // namespace mso::app
