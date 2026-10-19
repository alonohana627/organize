#include "mso/search/sample_rows.hpp"

#include <algorithm>
#include <format>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

#include "mso/domain/taxonomy.hpp"
#include "mso/search/detail/predicate_support.hpp"

namespace mso::search {

std::string_view to_string(RowAvailability availability) noexcept {
    switch (availability) {
        case RowAvailability::Available:
            return "available";
        case RowAvailability::SourceOffline:
            return "source offline";
        case RowAvailability::Missing:
            return "missing";
        case RowAvailability::Unknown:
            return "unknown";
    }
    return "unknown";
}

namespace {

/// Base columns of a row, in the order read_base_row() expects.
constexpr std::string_view kRowColumns =
    "s.id, s.uuid, s.display_name, s.rating, s.favorite, s.created_at, s.category_id, e.format, e.codec, "
    "e.duration_us, e.duration_exact, e.sample_rate, e.channels, e.bit_depth, e.byte_size, e.bpm_milli, "
    "e.bpm_source, e.bpm_confidence, e.key_class, e.key_mode, e.key_source, e.key_confidence, "
    "COALESCE(e.analysis_flags,0)";

/// Restricts a statement to the ids of a JSON array bound as parameter 1.
constexpr std::string_view kIdFilter = "IN (SELECT value FROM json_each(?1))";

/// Reads the sample and effective-metadata columns of kRowColumns.
SampleRow read_base_row(const sqlite::PreparedStatement& statement, const uuids::uuid& library) {
    SampleRow row;
    row.library = library;
    row.sample_id = statement.column_int64(0);
    row.sample_uuid = uuids::uuid::from_string(statement.column_text(1)).value_or(uuids::uuid());
    row.display_name = statement.column_text(2);
    row.rating = static_cast<int>(statement.column_int64(3));
    row.favorite = statement.column_int64(4) != 0;
    row.created_at = statement.column_int64(5);
    row.category_id = statement.column_optional_int64(6);
    row.format = statement.column_optional_text(7).value_or("");
    row.codec = statement.column_optional_text(8).value_or("");
    row.duration_us = statement.column_optional_int64(9);
    row.duration_exact = statement.column_optional_int64(10).value_or(0) != 0;
    row.sample_rate = statement.column_optional_int64(11);
    row.channels = statement.column_optional_int64(12);
    row.bit_depth = statement.column_optional_int64(13);
    row.byte_size = statement.column_optional_int64(14);
    row.bpm_milli = statement.column_optional_int64(15);
    row.bpm_source = statement.column_optional_text(16).value_or("");
    row.bpm_confidence = statement.column_optional_double(17);
    if (auto pitch_class = statement.column_optional_int64(18)) {
        const auto mode =
            parse_key_mode(statement.column_optional_text(19).value_or("unknown")).value_or(KeyMode::Unknown);
        row.key = MusicalKey{static_cast<int>(*pitch_class), mode};
    }
    row.key_source = statement.column_optional_text(20).value_or("");
    row.key_confidence = statement.column_optional_double(21);
    row.analysis_flags = statement.column_int64(22);
    return row;
}

/// The facts of a root that a row shows.
struct RootFacts {
    std::string name;
    uuids::uuid uuid;
    catalog::RootMode mode = catalog::RootMode::Referenced;
    bool reachable = false;  ///< not offline and not removed
    bool case_sensitive = false;
};

/// Every root of the library by id (a library has few roots).
Result<std::unordered_map<std::int64_t, RootFacts>> load_roots(sqlite::Connection& connection) {
    auto statement =
        connection.prepare("SELECT id, name, uuid, mode, availability, deleted, case_sensitive FROM roots");
    if (!statement) return std::unexpected(statement.error());
    std::unordered_map<std::int64_t, RootFacts> roots;
    while (true) {
        auto has_row = statement->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) break;
        RootFacts facts;
        facts.name = statement->column_text(1);
        facts.uuid = uuids::uuid::from_string(statement->column_text(2)).value_or(uuids::uuid());
        facts.mode = catalog::parse_root_mode(statement->column_text(3)).value_or(catalog::RootMode::Referenced);
        facts.reachable = statement->column_text(4) != "offline" && statement->column_int64(5) == 0;
        facts.case_sensitive = statement->column_int64(6) != 0;
        roots.emplace(statement->column_int64(0), std::move(facts));
    }
    return roots;
}

/// Display paths ("Drums/Kick") of tag or category nodes, built from one read of the tree.
/// Includes tombstoned nodes, so a path can always be built.
class TaxonomyPaths {
public:
    /// Reads the nodes of `table` ("tags" or "categories"): all of them, or only the nodes listed in
    /// `seed_ids` and their ancestors.
    Result<void> load(sqlite::Connection& connection, std::string_view table,
                      const std::optional<std::vector<std::int64_t>>& seed_ids) {
        const std::string sql = seed_ids
                                    ? std::format(
                                          "WITH RECURSIVE chain(id,depth) AS (SELECT value,0 FROM json_each(?1) UNION "
                                          "SELECT t.parent_id,c.depth+1 FROM {0} t JOIN chain c ON t.id=c.id "
                                          "WHERE t.parent_id IS NOT NULL AND c.depth<{1}) "
                                          "SELECT id, parent_id, name FROM {0} WHERE id IN (SELECT id FROM chain)",
                                          table, kMaxTaxonomyDepth)
                                    : std::format("SELECT id, parent_id, name FROM {}", table);
        auto statement = connection.prepare(sql);
        if (!statement) return std::unexpected(statement.error());
        if (seed_ids) statement->bind(1, detail::id_array_json(*seed_ids));
        while (true) {
            auto has_row = statement->next_row();
            if (!has_row) return std::unexpected(has_row.error());
            if (!*has_row) break;
            nodes_.emplace(statement->column_int64(0),
                           Node{statement->column_optional_int64(1), statement->column_text(2)});
        }
        return {};
    }

    /// The display path of a node ("" for an unknown id); memoized. Walks at most the taxonomy
    /// depth limit, so a corrupt parent cycle cannot loop.
    const std::string& path(std::int64_t id) {
        if (auto cached = paths_.find(id); cached != paths_.end()) return cached->second;
        std::vector<std::int64_t> uncached;  // from `id` upwards
        std::string prefix;
        std::int64_t current = id;
        while (uncached.size() <= kMaxTaxonomyDepth) {
            if (auto cached = paths_.find(current); cached != paths_.end()) {
                prefix = cached->second;
                break;
            }
            const auto node = nodes_.find(current);
            if (node == nodes_.end()) break;
            uncached.push_back(current);
            if (!node->second.parent_id) break;
            current = *node->second.parent_id;
        }
        for (auto ancestor = uncached.rbegin(); ancestor != uncached.rend(); ++ancestor) {
            const std::string& name = nodes_.at(*ancestor).name;
            prefix = prefix.empty() ? name : prefix + '/' + name;
            paths_.insert_or_assign(*ancestor, prefix);
        }
        return paths_.try_emplace(id).first->second;
    }

private:
    struct Node {
        std::optional<std::int64_t> parent_id;
        std::string name;
    };
    std::unordered_map<std::int64_t, Node> nodes_;
    std::unordered_map<std::int64_t, std::string> paths_;
};

/// Fills the primary-location fields of a row from its first-ranked live location.
void apply_location(SampleRow& row, std::int64_t root_id, std::string relative_path, std::string stored_path_key,
                    std::string_view state, const std::unordered_map<std::int64_t, RootFacts>& roots) {
    row.root_id = root_id;
    row.relative_path = std::move(relative_path);
    row.path_key = std::move(stored_path_key);
    const auto slash = row.relative_path.rfind('/');
    row.file_name = slash == std::string::npos ? row.relative_path : row.relative_path.substr(slash + 1);
    row.folder_path = slash == std::string::npos ? std::string() : row.relative_path.substr(0, slash);
    bool reachable = false;
    if (const auto root = roots.find(root_id); root != roots.end()) {
        row.root_name = root->second.name;
        row.root_uuid = root->second.uuid;
        row.root_mode = root->second.mode;
        row.root_case_sensitive = root->second.case_sensitive;
        reachable = root->second.reachable;
    }
    if (state == "present")
        row.availability = reachable ? RowAvailability::Available : RowAvailability::SourceOffline;
    else if (state == "missing")
        row.availability = RowAvailability::Missing;
}

/// Fills category and tag paths from ids; tags are sorted by path with `tag_ids` kept parallel.
void apply_taxonomy(SampleRow& row, TaxonomyPaths& tags, TaxonomyPaths& categories) {
    if (row.category_id) row.category = categories.path(*row.category_id);
    std::vector<std::size_t> order(row.tag_ids.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::vector<std::string> paths;
    paths.reserve(row.tag_ids.size());
    for (const auto id : row.tag_ids)
        paths.push_back(tags.path(id));
    std::ranges::sort(order, [&](std::size_t left, std::size_t right) {
        return paths[left] != paths[right] ? paths[left] < paths[right] : row.tag_ids[left] < row.tag_ids[right];
    });
    std::vector<std::int64_t> sorted_ids;
    sorted_ids.reserve(order.size());
    row.tag_paths.clear();
    for (const auto index : order) {
        sorted_ids.push_back(row.tag_ids[index]);
        row.tag_paths.push_back(std::move(paths[index]));
    }
    row.tag_ids = std::move(sorted_ids);
}

/// A statement whose rows are ordered by sample id (column 0), consumed in step with the base rows.
class OrderedCursor {
public:
    explicit OrderedCursor(sqlite::PreparedStatement& statement) : statement_(statement) {}

    /// Calls `take(statement)` for every row of `sample_id`, skipping rows of smaller ids.
    template <class Take>
    Result<void> for_sample(std::int64_t sample_id, Take&& take) {
        if (!started_) {
            started_ = true;
            if (auto advanced = advance(); !advanced) return advanced;
        }
        while (has_row_ && statement_.column_int64(0) < sample_id)
            if (auto advanced = advance(); !advanced) return advanced;
        while (has_row_ && statement_.column_int64(0) == sample_id) {
            take(statement_);
            if (auto advanced = advance(); !advanced) return advanced;
        }
        return {};
    }

private:
    Result<void> advance() {
        auto has_row = statement_.next_row();
        if (!has_row) return std::unexpected(has_row.error());
        has_row_ = *has_row;
        return {};
    }

    sqlite::PreparedStatement& statement_;
    bool started_ = false;
    bool has_row_ = false;
};

/// Streams base rows (with primary location and tag ids, but no taxonomy paths yet) in ascending
/// id order: three ordered statements merged in memory. `id_filter` restricts all three to the
/// ids of a JSON array.
Result<void> stream_rows(sqlite::Connection& connection, const uuids::uuid& library,
                         const std::optional<std::string>& id_filter,
                         const std::unordered_map<std::int64_t, RootFacts>& roots,
                         const std::function<void(SampleRow&&)>& emit) {
    const std::string filter_samples = id_filter ? std::format(" AND s.id {}", kIdFilter) : std::string();
    const std::string filter_locations = id_filter ? std::format(" AND l.sample_id {}", kIdFilter) : std::string();
    const std::string filter_tags = id_filter ? std::format(" WHERE st.sample_id {}", kIdFilter) : std::string();
    auto samples =
        connection.prepare(std::format("SELECT {} FROM samples s LEFT JOIN effective_metadata e ON e.sample_id=s.id "
                                       "WHERE s.deleted=0{} ORDER BY s.id",
                                       kRowColumns, filter_samples));
    if (!samples) return std::unexpected(samples.error());
    auto locations = connection.prepare(
        std::format("SELECT l.sample_id, l.root_id, l.relative_path, l.path_key, l.state FROM locations l "
                    "JOIN roots r ON r.id=l.root_id WHERE l.state<>'retired'{} ORDER BY l.sample_id, {}",
                    filter_locations, detail::kPrimaryLocationOrder));
    if (!locations) return std::unexpected(locations.error());
    auto tags = connection.prepare(std::format(
        "SELECT st.sample_id, st.tag_id FROM sample_tags st{} ORDER BY st.sample_id, st.tag_id", filter_tags));
    if (!tags) return std::unexpected(tags.error());
    if (id_filter) {
        samples->bind(1, *id_filter);
        locations->bind(1, *id_filter);
        tags->bind(1, *id_filter);
    }

    OrderedCursor location_cursor(*locations);
    OrderedCursor tag_cursor(*tags);
    while (true) {
        auto has_row = samples->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) break;
        SampleRow row = read_base_row(*samples, library);
        bool primary_seen = false;
        auto located = location_cursor.for_sample(row.sample_id, [&](const sqlite::PreparedStatement& location) {
            if (primary_seen) return;  // ranked first by kPrimaryLocationOrder
            primary_seen = true;
            apply_location(row, location.column_int64(1), location.column_text(2), location.column_text(3),
                           location.column_text(4), roots);
        });
        if (!located) return located;
        auto tagged = tag_cursor.for_sample(
            row.sample_id, [&](const sqlite::PreparedStatement& tag) { row.tag_ids.push_back(tag.column_int64(1)); });
        if (!tagged) return tagged;
        emit(std::move(row));
    }
    return {};
}

}  // namespace

Result<std::vector<SampleRow>> load_rows(catalog::Library& library, std::span<const std::int64_t> sample_ids) {
    std::vector<std::int64_t> unique_ids;
    unique_ids.reserve(sample_ids.size());
    std::unordered_set<std::int64_t> seen;
    for (const auto id : sample_ids)
        if (seen.insert(id).second) unique_ids.push_back(id);
    if (unique_ids.empty()) return std::vector<SampleRow>{};

    const uuids::uuid library_uuid = library.uuid();
    auto reader = library.acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto& connection = reader->connection();
    if (auto begun = connection.execute_script("BEGIN"); !begun) return std::unexpected(begun.error());
    auto roots = load_roots(connection);
    if (!roots) return std::unexpected(roots.error());

    std::vector<SampleRow> loaded;
    loaded.reserve(unique_ids.size());
    auto streamed = stream_rows(connection, library_uuid, detail::id_array_json(unique_ids), *roots,
                                [&](SampleRow&& row) { loaded.push_back(std::move(row)); });
    if (!streamed) return std::unexpected(streamed.error());

    // Read only the tag and category nodes these rows need, with their ancestors.
    std::vector<std::int64_t> tag_ids;
    std::vector<std::int64_t> category_ids;
    for (const auto& row : loaded) {
        tag_ids.insert(tag_ids.end(), row.tag_ids.begin(), row.tag_ids.end());
        if (row.category_id) category_ids.push_back(*row.category_id);
    }
    TaxonomyPaths tag_paths;
    TaxonomyPaths category_paths;
    if (!tag_ids.empty())
        if (auto read = tag_paths.load(connection, "tags", tag_ids); !read) return std::unexpected(read.error());
    if (!category_ids.empty())
        if (auto read = category_paths.load(connection, "categories", category_ids); !read)
            return std::unexpected(read.error());

    std::unordered_map<std::int64_t, std::size_t> position;
    position.reserve(loaded.size());
    for (std::size_t index = 0; index < loaded.size(); ++index) {
        apply_taxonomy(loaded[index], tag_paths, category_paths);
        position.emplace(loaded[index].sample_id, index);
    }
    std::vector<SampleRow> ordered;
    ordered.reserve(loaded.size());
    for (const auto id : unique_ids)
        if (const auto found = position.find(id); found != position.end())
            ordered.push_back(std::move(loaded[found->second]));
    return ordered;
}

Result<void> for_each_row(catalog::Library& library, const std::function<void(SampleRow&&)>& visit) {
    const uuids::uuid library_uuid = library.uuid();
    auto reader = library.acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto& connection = reader->connection();
    // One snapshot for the roots, the taxonomy and the three streamed statements.
    if (auto begun = connection.execute_script("BEGIN"); !begun) return std::unexpected(begun.error());
    auto roots = load_roots(connection);
    if (!roots) return std::unexpected(roots.error());
    TaxonomyPaths tag_paths;
    TaxonomyPaths category_paths;
    if (auto read = tag_paths.load(connection, "tags", std::nullopt); !read) return std::unexpected(read.error());
    if (auto read = category_paths.load(connection, "categories", std::nullopt); !read)
        return std::unexpected(read.error());
    return stream_rows(connection, library_uuid, std::nullopt, *roots, [&](SampleRow&& row) {
        apply_taxonomy(row, tag_paths, category_paths);
        visit(std::move(row));
    });
}

Result<TagAliases> load_tag_aliases(catalog::Library& library) {
    TagAliases aliases;
    aliases.library = library.uuid();
    auto reader = library.acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto& connection = reader->connection();
    if (auto begun = connection.execute_script("BEGIN"); !begun) return std::unexpected(begun.error());
    // Every aliased (merged) tag's folded name with the end of its redirect chain: the tag that has
    // no alias itself. Chains are bounded like the catalog's canonical_tag(); a cycle yields nothing.
    auto statement = connection.prepare(std::format(
        "WITH RECURSIVE redirect(name_key,id,hops) AS ("
        "SELECT t.name_key, a.canonical_tag_id, 1 FROM tag_aliases a JOIN tags t ON t.id=a.source_tag_id "
        "UNION SELECT redirect.name_key, a.canonical_tag_id, redirect.hops+1 FROM redirect "
        "JOIN tag_aliases a ON a.source_tag_id=redirect.id WHERE redirect.hops<={0}) "
        "SELECT DISTINCT redirect.name_key, redirect.id FROM redirect JOIN tags canonical ON canonical.id=redirect.id "
        "WHERE canonical.deleted=0 AND redirect.id NOT IN (SELECT source_tag_id FROM tag_aliases) "
        "ORDER BY redirect.name_key, redirect.id",
        kMaxTaxonomyDepth));
    if (!statement) return std::unexpected(statement.error());
    std::vector<std::pair<std::string, std::int64_t>> redirects;
    while (true) {
        auto has_row = statement->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) break;
        redirects.emplace_back(statement->column_text(0), statement->column_int64(1));
    }
    if (redirects.empty()) return aliases;
    std::vector<std::int64_t> canonical_ids;
    for (const auto& [name_key, id] : redirects)
        canonical_ids.push_back(id);
    TaxonomyPaths paths;
    if (auto read = paths.load(connection, "tags", canonical_ids); !read) return std::unexpected(read.error());
    for (const auto& [name_key, id] : redirects)
        aliases.canonical_paths[name_key].push_back(paths.path(id));
    return aliases;
}

}  // namespace mso::search
