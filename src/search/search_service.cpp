#include "mso/search/search_service.hpp"

#include <picosha2.h>

#include <algorithm>
#include <format>
#include <map>

#include <nlohmann/json.hpp>

#include "mso/catalog/taxonomy_lookup.hpp"
#include "mso/search/query_compiler.hpp"

namespace mso::search {

std::string_view to_string(SortField field) noexcept {
    switch (field) {
        case SortField::Name:
            return "name";
        case SortField::Added:
            return "added";
        case SortField::Duration:
            return "duration";
        case SortField::Size:
            return "size";
        case SortField::SampleRate:
            return "rate";
        case SortField::Bpm:
            return "bpm";
        case SortField::Key:
            return "key";
        case SortField::Rating:
            return "rating";
        case SortField::Format:
            return "format";
        case SortField::Relevance:
            return "relevance";
    }
    return "name";
}

std::optional<SortField> parse_sort_field(std::string_view text) noexcept {
    for (const auto field :
         {SortField::Name, SortField::Added, SortField::Duration, SortField::Size, SortField::SampleRate,
          SortField::Bpm, SortField::Key, SortField::Rating, SortField::Format, SortField::Relevance})
        if (to_string(field) == text) return field;
    return std::nullopt;
}

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

constexpr int kCursorVersion = 1;

/// SQL expression for each sort field; text sorts compare normalized keys.
std::string_view sort_expression(SortField field) {
    switch (field) {
        case SortField::Name:
            return "s.name_key";
        case SortField::Added:
            return "s.created_at";
        case SortField::Duration:
            return "e.duration_us";
        case SortField::Size:
            return "e.byte_size";
        case SortField::SampleRate:
            return "e.sample_rate";
        case SortField::Bpm:
            return "e.bpm_milli";
        case SortField::Key:
            return "(e.key_class*2 + CASE e.key_mode WHEN 'minor' THEN 1 ELSE 0 END)";
        case SortField::Rating:
            return "s.rating";
        case SortField::Format:
            return "e.format";
        case SortField::Relevance:
            return "s.name_key";
    }
    return "s.name_key";
}

constexpr std::string_view kRowColumns =
    "s.id, s.uuid, s.display_name, s.rating, s.favorite, s.created_at, s.category_id, e.format, e.codec, "
    "e.duration_us, e.duration_exact, e.sample_rate, e.channels, e.bit_depth, e.byte_size, e.bpm_milli, "
    "e.bpm_source, e.bpm_confidence, e.key_class, e.key_mode, e.key_source, e.key_confidence, "
    "COALESCE(e.analysis_flags,0)";
constexpr int kSortValueColumn = 23;  // appended after kRowColumns

/// A row together with the values that order it globally across libraries.
struct OrderedRow {
    SampleRow row;
    int partition = 0;  ///< 0 = sort value present, 1 = null
    nlohmann::json sort_value;
};

/// Keyset position of one library inside a federated cursor.
struct LibraryPosition {
    int partition = 0;
    nlohmann::json value;
    std::int64_t last_id = 0;
    bool started = false;
    bool exhausted = false;
};

std::string query_fingerprint(const SearchRequest& request) {
    nlohmann::json scope = {
        {"root", request.scope.root_id ? nlohmann::json(*request.scope.root_id) : nlohmann::json()},
        {"folder", request.scope.folder ? nlohmann::json(*request.scope.folder) : nlohmann::json()},
        {"collection", request.scope.collection_id ? nlohmann::json(*request.scope.collection_id) : nlohmann::json()},
        {"tag", request.scope.tag_id ? nlohmann::json(*request.scope.tag_id) : nlohmann::json()},
        {"category", request.scope.category_id ? nlohmann::json(*request.scope.category_id) : nlohmann::json()},
        {"uncategorized", request.scope.uncategorized}};
    return picosha2::hash256_hex_string(nlohmann::json{
        {"text", request.text},
        {"scope", scope},
        {"sort", to_string(request.sort)},
        {"descending", request.descending}}.dump());
}

/// Compiles the navigation scope into SQL for one library.
Result<SqlPredicate> compile_scope(const Scope& scope, sqlite::Connection& connection) {
    SqlPredicate out;
    std::vector<std::string> parts;
    if (scope.root_id) {
        if (scope.folder && !scope.folder->empty()) {
            parts.emplace_back(
                "EXISTS(SELECT 1 FROM locations sl WHERE sl.sample_id=s.id AND sl.root_id=? "
                "AND sl.state<>'retired' AND sl.path_key LIKE ? ESCAPE '\\')");
            out.parameters.emplace_back(*scope.root_id);
            std::string key = fold_path_key(*scope.folder);
            std::string escaped;
            for (const char character : key) {
                if (character == '%' || character == '_' || character == '\\') escaped.push_back('\\');
                escaped.push_back(character);
            }
            out.parameters.emplace_back(escaped + "/%");
        } else {
            parts.emplace_back(
                "EXISTS(SELECT 1 FROM locations sl WHERE sl.sample_id=s.id AND sl.root_id=? "
                "AND sl.state<>'retired')");
            out.parameters.emplace_back(*scope.root_id);
        }
    }
    if (scope.collection_id) {
        auto kind = connection.prepare("SELECT kind, query_json FROM collections WHERE id=?1 AND deleted=0");
        if (!kind) return std::unexpected(kind.error());
        kind->bind(1, *scope.collection_id);
        auto has_row = kind->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) {
            parts.emplace_back("0");
        } else if (kind->column_text(0) == "manual") {
            parts.emplace_back(
                "EXISTS(SELECT 1 FROM collection_items ci WHERE ci.collection_id=? AND ci.sample_id=s.id)");
            out.parameters.emplace_back(*scope.collection_id);
        } else {
            const auto document = nlohmann::json::parse(kind->column_optional_text(1).value_or("{}"), nullptr, false);
            const std::string text = document.is_object() ? document.value("text", std::string()) : std::string();
            kind->reset();
            auto parsed = parse_query(text);
            if (!parsed) return std::unexpected(parsed.error());
            auto compiled = compile_query(*parsed, connection);
            if (!compiled) return std::unexpected(compiled.error());
            parts.push_back('(' + compiled->sql + ')');
            out.parameters.insert(out.parameters.end(), compiled->parameters.begin(), compiled->parameters.end());
        }
    }
    auto subtree = [](std::string_view table) {
        return std::format(
            "WITH RECURSIVE subtree(id,depth) AS (SELECT ?,0 UNION ALL SELECT t.id,subtree.depth+1 "
            "FROM {} t JOIN subtree ON t.parent_id=subtree.id WHERE subtree.depth<16) SELECT id FROM subtree",
            table);
    };
    if (scope.tag_id) {
        parts.push_back(std::format(
            "EXISTS(SELECT 1 FROM sample_tags st WHERE st.sample_id=s.id AND st.tag_id IN ({}))", subtree("tags")));
        out.parameters.emplace_back(*scope.tag_id);
    }
    if (scope.category_id) {
        parts.push_back(std::format("s.category_id IN ({})", subtree("categories")));
        out.parameters.emplace_back(*scope.category_id);
    }
    if (scope.uncategorized) parts.emplace_back("s.category_id IS NULL");

    out.sql = "1";
    for (const auto& part : parts)
        out.sql += " AND " + part;
    return out;
}

/// Fills location, availability, tags and category for hydrated rows.
Result<void> hydrate_relations(sqlite::Connection& connection, SampleRow& row, std::optional<std::int64_t> category_id,
                               std::map<std::int64_t, std::string>& tag_path_cache) {
    auto location = connection.prepare(
        "SELECT l.relative_path, l.state, r.name, r.uuid, r.mode, r.availability FROM locations l "
        "JOIN roots r ON r.id=l.root_id WHERE l.sample_id=?1 AND l.state<>'retired' "
        "ORDER BY (l.state='present' AND r.availability<>'offline') DESC, l.state='present' DESC, l.id LIMIT 1");
    if (!location) return std::unexpected(location.error());
    location->bind(1, row.sample_id);
    auto has_row = location->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (*has_row) {
        row.relative_path = location->column_text(0);
        const std::string state = location->column_text(1);
        row.root_name = location->column_text(2);
        row.root_uuid = uuids::uuid::from_string(location->column_text(3)).value_or(uuids::uuid());
        row.root_mode = catalog::parse_root_mode(location->column_text(4)).value_or(catalog::RootMode::Referenced);
        const std::string root_availability = location->column_text(5);
        if (state == "present")
            row.availability =
                root_availability == "offline" ? RowAvailability::SourceOffline : RowAvailability::Available;
        else if (state == "missing")
            row.availability = RowAvailability::Missing;
        const auto slash = row.relative_path.rfind('/');
        row.file_name = slash == std::string::npos ? row.relative_path : row.relative_path.substr(slash + 1);
        location->reset();
    }

    auto tags = connection.prepare("SELECT tag_id FROM sample_tags WHERE sample_id=?1");
    if (!tags) return std::unexpected(tags.error());
    tags->bind(1, row.sample_id);
    std::vector<std::string> paths;
    while (true) {
        auto next = tags->next_row();
        if (!next) return std::unexpected(next.error());
        if (!*next) break;
        const auto tag_id = tags->column_int64(0);
        auto cached = tag_path_cache.find(tag_id);
        if (cached == tag_path_cache.end()) {
            auto path = connection.prepare(
                "WITH RECURSIVE chain(id,parent_id,name,depth) AS (SELECT id,parent_id,name,0 FROM tags WHERE id=?1 "
                "UNION ALL SELECT t.id,t.parent_id,t.name,c.depth+1 FROM tags t JOIN chain c ON t.id=c.parent_id "
                "WHERE c.depth<16) SELECT group_concat(name,'/') FROM (SELECT name FROM chain ORDER BY depth DESC)");
            if (!path) return std::unexpected(path.error());
            path->bind(1, tag_id);
            auto found = path->next_row();
            if (!found) return std::unexpected(found.error());
            cached = tag_path_cache.emplace(tag_id, *found ? path->column_optional_text(0).value_or("") : "").first;
        }
        paths.push_back(cached->second);
    }
    std::ranges::sort(paths);
    for (const auto& path : paths)
        row.tags += (row.tags.empty() ? "" : ", ") + path;
    row.tag_paths = std::move(paths);

    if (category_id) {
        auto category = connection.prepare(
            "WITH RECURSIVE chain(id,parent_id,name,depth) AS (SELECT id,parent_id,name,0 FROM categories WHERE id=?1 "
            "UNION ALL SELECT c.id,c.parent_id,c.name,h.depth+1 FROM categories c JOIN chain h ON c.id=h.parent_id "
            "WHERE h.depth<16) SELECT group_concat(name,'/') FROM (SELECT name FROM chain ORDER BY depth DESC)");
        if (!category) return std::unexpected(category.error());
        category->bind(1, *category_id);
        auto found = category->next_row();
        if (!found) return std::unexpected(found.error());
        if (*found) row.category = category->column_optional_text(0).value_or("");
    }
    return {};
}

/// Reads the base columns of a result row.
SampleRow read_base_row(const sqlite::PreparedStatement& statement, const uuids::uuid& library) {
    SampleRow row;
    row.library = library;
    row.sample_id = statement.column_int64(0);
    row.sample_uuid = uuids::uuid::from_string(statement.column_text(1)).value_or(uuids::uuid());
    row.display_name = statement.column_text(2);
    row.rating = static_cast<int>(statement.column_int64(3));
    row.favorite = statement.column_int64(4) != 0;
    row.created_at = statement.column_int64(5);
    row.format = statement.column_optional_text(7).value_or("");
    row.codec = statement.column_optional_text(8).value_or("");
    row.duration_us = statement.column_optional_int64(9);
    row.duration_exact = statement.column_int64(10) != 0;
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

/// True for sorts whose key is text (compared as normalized strings); others are integers.
bool is_text_sort(SortField field) {
    return field == SortField::Name || field == SortField::Format;
}

/// The sort key of the current row as JSON (null, integer or string) for cursors and merging.
nlohmann::json sort_value_json(const sqlite::PreparedStatement& statement, SortField field) {
    if (statement.column_is_null(kSortValueColumn)) return nlohmann::json();
    if (is_text_sort(field)) return nlohmann::json(statement.column_text(kSortValueColumn));
    return nlohmann::json(statement.column_int64(kSortValueColumn));
}

/// Everything needed to run one library's part of a request.
struct LibraryQuery {
    catalog::Library* library = nullptr;
    LibraryCoverage coverage;
};

/// Runs the filtered, keyset-paged query of one library, filling up to `limit` rows across
/// the non-null and null partitions.
Result<std::vector<OrderedRow>> fetch_library_page(catalog::Library& library, const Query& query,
                                                   const SearchRequest& request, const LibraryPosition& position,
                                                   std::size_t limit) {
    auto reader = library.acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto& connection = reader->connection();
    auto filter = compile_query(query, connection);
    if (!filter) return std::unexpected(filter.error());
    auto scope = compile_scope(request.scope, connection);
    if (!scope) return std::unexpected(scope.error());

    const std::string expression(sort_expression(request.sort));
    const std::string_view direction = request.descending ? "DESC" : "ASC";
    const std::string_view after = request.descending ? "<" : ">";
    const uuids::uuid library_uuid = library.identity().uuid;
    std::vector<OrderedRow> rows;
    std::map<std::int64_t, std::string> tag_path_cache;

    for (int partition = position.partition; partition <= 1 && rows.size() < limit; ++partition) {
        const bool resume = position.started && partition == position.partition && !rows.size();
        std::string keyset;
        if (partition == 0) {
            keyset = std::format("{} IS NOT NULL", expression);
            if (resume) keyset += std::format(" AND ({0} {1} ? OR ({0} = ? AND s.id {1} ?))", expression, after);
        } else {
            keyset = std::format("{} IS NULL", expression);
            if (resume) keyset += std::format(" AND s.id {} ?", after);
        }
        const std::string sql = std::format(
            "SELECT {0}, {1} FROM samples s LEFT JOIN effective_metadata e ON e.sample_id=s.id "
            "WHERE s.deleted=0 AND ({2}) AND ({3}) AND {4} ORDER BY {1} {5}, s.id {5} LIMIT ?",
            kRowColumns, expression, filter->sql, scope->sql, keyset, direction);
        auto statement = connection.prepare(sql);
        if (!statement) return std::unexpected(statement.error());
        int next = bind_parameters(*statement, filter->parameters, 1);
        next = bind_parameters(*statement, scope->parameters, next);
        if (resume && partition == 0) {
            const nlohmann::json& value = position.value;
            for (int repeat = 0; repeat < 2; ++repeat) {
                if (value.is_number_integer())
                    statement->bind(next++, value.get<std::int64_t>());
                else
                    statement->bind(next++, value.is_string() ? value.get<std::string>() : std::string());
            }
            statement->bind(next++, position.last_id);
        } else if (resume) {
            statement->bind(next++, position.last_id);
        }
        statement->bind(next, static_cast<std::int64_t>(limit - rows.size()));
        while (true) {
            auto has_row = statement->next_row();
            if (!has_row) return std::unexpected(has_row.error());
            if (!*has_row) break;
            OrderedRow ordered{read_base_row(*statement, library_uuid), partition,
                               sort_value_json(*statement, request.sort)};
            const auto category_id = statement->column_optional_int64(6);
            if (auto hydrated = hydrate_relations(connection, ordered.row, category_id, tag_path_cache); !hydrated)
                return std::unexpected(hydrated.error());
            rows.push_back(std::move(ordered));
        }
    }
    return rows;
}

/// Global order across libraries: partition, value (by direction), library UUID, id.
bool comes_before(const OrderedRow& left, const OrderedRow& right, bool descending) {
    if (left.partition != right.partition) return left.partition < right.partition;
    if (left.sort_value != right.sort_value)
        return descending ? right.sort_value < left.sort_value : left.sort_value < right.sort_value;
    const auto left_library = uuids::to_string(left.row.library);
    const auto right_library = uuids::to_string(right.row.library);
    if (left_library != right_library) return left_library < right_library;
    return descending ? left.row.sample_id > right.row.sample_id : left.row.sample_id < right.row.sample_id;
}

Result<std::map<std::string, LibraryPosition>> decode_cursor(const std::optional<Cursor>& cursor,
                                                             const std::string& fingerprint) {
    std::map<std::string, LibraryPosition> positions;
    if (!cursor) return positions;
    const auto document = nlohmann::json::parse(cursor->token, nullptr, false);
    if (!document.is_object() || document.value("v", 0) != kCursorVersion ||
        document.value("q", std::string()) != fingerprint)
        return fail(ErrorCode::StaleCursor, "the result set changed; refresh the view");
    for (const auto& [library, state] : document["libraries"].items()) {
        LibraryPosition position;
        position.partition = state.value("p", 0);
        position.value = state.contains("v") ? state["v"] : nlohmann::json();
        position.last_id = state.value("id", std::int64_t{0});
        position.started = state.value("started", false);
        position.exhausted = state.value("done", false);
        positions.emplace(library, std::move(position));
    }
    if (document.contains("offset")) positions["@offset"].last_id = document["offset"].get<std::int64_t>();
    return positions;
}

std::size_t clamp_page_size(std::size_t requested) {
    return std::clamp<std::size_t>(requested, 1, SearchService::kMaxPageSize);
}

}  // namespace

// ---------------------------------------------------------------- SearchService

Result<void> SearchService::validate(std::string_view text) const {
    auto parsed = parse_query(text);
    if (!parsed) return std::unexpected(parsed.error());
    return {};
}

Result<SearchPage> SearchService::search(std::span<catalog::Library* const> libraries,
                                         const SearchRequest& request) const {
    auto query = parse_query(request.text);
    if (!query) return std::unexpected(query.error());
    const std::size_t page_size = clamp_page_size(request.page_size);
    const std::string fingerprint = query_fingerprint(request);
    auto positions = decode_cursor(request.cursor, fingerprint);
    if (!positions) return std::unexpected(positions.error());

    SearchPage page;
    const auto ranked_terms = positive_text_terms(*query);
    if (request.sort == SortField::Relevance && !ranked_terms.empty()) {
        // Bounded top-K per library by BM25, fused across libraries with reciprocal rank fusion.
        std::string match;
        for (const auto& term : ranked_terms)
            match += (match.empty() ? "" : " OR ") + fts_expression(term);
        std::vector<SampleRow> fused;
        for (auto* library : libraries) {
            LibraryCoverage coverage{library->identity().uuid, library->identity().name, true, std::nullopt};
            auto reader = library->acquire_reader();
            auto filter = reader ? compile_query(*query, reader->connection())
                                 : Result<SqlPredicate>(std::unexpected(reader.error()));
            auto scope = reader && filter ? compile_scope(request.scope, reader->connection())
                                          : Result<SqlPredicate>(SqlPredicate{});
            if (!reader || !filter || !scope) {
                coverage.complete = false;
                coverage.error = !reader ? reader.error() : !filter ? filter.error() : scope.error();
                page.coverage.push_back(std::move(coverage));
                continue;
            }
            const std::string sql = std::format(
                "SELECT {0}, ranked.score FROM (SELECT rowid AS id, bm25(sample_fts) AS score FROM sample_fts "
                "WHERE sample_fts MATCH ?) ranked JOIN samples s ON s.id=ranked.id LEFT JOIN effective_metadata e "
                "ON e.sample_id=s.id WHERE s.deleted=0 AND ({1}) AND ({2}) ORDER BY ranked.score, s.id LIMIT ?",
                kRowColumns, filter->sql, scope->sql);
            auto statement = reader->connection().prepare(sql);
            if (!statement) return std::unexpected(statement.error());
            statement->bind(1, match);
            int next = bind_parameters(*statement, filter->parameters, 2);
            next = bind_parameters(*statement, scope->parameters, next);
            statement->bind(next, static_cast<std::int64_t>(kRelevanceDepth));
            std::map<std::int64_t, std::string> cache;
            for (std::size_t rank = 1;; ++rank) {
                auto has_row = statement->next_row();
                if (!has_row) return std::unexpected(has_row.error());
                if (!*has_row) break;
                SampleRow row = read_base_row(*statement, coverage.library);
                row.relevance = 1.0 / (kReciprocalRankConstant + static_cast<double>(rank));
                if (auto hydrated =
                        hydrate_relations(reader->connection(), row, statement->column_optional_int64(6), cache);
                    !hydrated)
                    return std::unexpected(hydrated.error());
                if (rank == kRelevanceDepth) page.relevance_window_limited = true;
                fused.push_back(std::move(row));
            }
            page.coverage.push_back(std::move(coverage));
        }
        std::ranges::stable_sort(fused, [](const SampleRow& left, const SampleRow& right) {
            if (left.relevance != right.relevance) return left.relevance > right.relevance;
            if (uuids::to_string(left.library) != uuids::to_string(right.library))
                return uuids::to_string(left.library) < uuids::to_string(right.library);
            return left.sample_id < right.sample_id;
        });
        const auto offset = static_cast<std::size_t>((*positions)["@offset"].last_id);
        for (std::size_t index = offset; index < fused.size() && page.rows.size() < page_size; ++index)
            page.rows.push_back(std::move(fused[index]));
        if (offset + page.rows.size() < fused.size())
            page.next = Cursor{nlohmann::json{{"v", kCursorVersion},
                                              {"q", fingerprint},
                                              {"libraries", nlohmann::json::object()},
                                              {"offset", offset + page.rows.size()}}
                                   .dump()};
        return page;
    }

    // Metadata sort: fetch a page from each library after its own position, then k-way merge.
    SearchRequest effective = request;
    if (effective.sort == SortField::Relevance) effective.sort = SortField::Name;
    std::vector<OrderedRow> merged;
    std::map<std::string, std::size_t> fetched_counts;
    for (auto* library : libraries) {
        const auto identity = library->identity();
        LibraryCoverage coverage{identity.uuid, identity.name, true, std::nullopt};
        const auto key = uuids::to_string(identity.uuid);
        const LibraryPosition position = positions->contains(key) ? positions->at(key) : LibraryPosition{};
        if (position.exhausted) {
            page.coverage.push_back(std::move(coverage));
            continue;
        }
        auto rows = fetch_library_page(*library, *query, effective, position, page_size);
        if (!rows) {
            if (rows.error().code == ErrorCode::InvalidArgument || rows.error().code == ErrorCode::ParseError)
                return std::unexpected(rows.error());
            coverage.complete = false;
            coverage.error = rows.error();
        } else {
            fetched_counts[key] = rows->size();
            for (auto& row : *rows)
                merged.push_back(std::move(row));
        }
        page.coverage.push_back(std::move(coverage));
    }
    std::ranges::stable_sort(merged, [&](const OrderedRow& left, const OrderedRow& right) {
        return comes_before(left, right, effective.descending);
    });
    if (merged.size() > page_size) merged.resize(page_size);

    // New per-library positions: the last row consumed from each; untouched libraries keep theirs.
    std::map<std::string, LibraryPosition> next_positions = *positions;
    next_positions.erase("@offset");
    std::map<std::string, std::size_t> consumed;
    for (const auto& ordered : merged) {
        const auto key = uuids::to_string(ordered.row.library);
        ++consumed[key];
        auto& position = next_positions[key];
        position.partition = ordered.partition;
        position.value = ordered.sort_value;
        position.last_id = ordered.row.sample_id;
        position.started = true;
    }
    bool more = false;
    for (const auto& [key, fetched] : fetched_counts) {
        const bool library_has_more = fetched == page_size || consumed[key] < fetched;
        next_positions[key].exhausted = !library_has_more;
        more = more || library_has_more;
    }
    for (auto& ordered : merged)
        page.rows.push_back(std::move(ordered.row));
    if (more) {
        nlohmann::json state = nlohmann::json::object();
        for (const auto& [key, position] : next_positions)
            state[key] = {{"p", position.partition},
                          {"v", position.value},
                          {"id", position.last_id},
                          {"started", position.started},
                          {"done", position.exhausted}};
        page.next = Cursor{nlohmann::json{{"v", kCursorVersion}, {"q", fingerprint}, {"libraries", state}}.dump()};
    }
    return page;
}

Result<CountResult> SearchService::count(std::span<catalog::Library* const> libraries,
                                         const SearchRequest& request) const {
    auto query = parse_query(request.text);
    if (!query) return std::unexpected(query.error());
    CountResult result;
    for (auto* library : libraries) {
        const auto identity = library->identity();
        LibraryCoverage coverage{identity.uuid, identity.name, true, std::nullopt};
        auto reader = library->acquire_reader();
        if (!reader) {
            coverage.complete = false;
            coverage.error = reader.error();
            result.coverage.push_back(std::move(coverage));
            continue;
        }
        auto filter = compile_query(*query, reader->connection());
        if (!filter) return std::unexpected(filter.error());
        auto scope = compile_scope(request.scope, reader->connection());
        if (!scope) return std::unexpected(scope.error());
        auto statement = reader->connection().prepare(
            std::format("SELECT count(*) FROM samples s LEFT JOIN effective_metadata e ON e.sample_id=s.id "
                        "WHERE s.deleted=0 AND ({}) AND ({})",
                        filter->sql, scope->sql));
        if (!statement) return std::unexpected(statement.error());
        bind_parameters(*statement, scope->parameters, bind_parameters(*statement, filter->parameters, 1));
        auto has_row = statement->next_row();
        if (!has_row) {
            coverage.complete = false;
            coverage.error = has_row.error();
        } else if (*has_row) {
            result.total += statement->column_int64(0);
        }
        result.coverage.push_back(std::move(coverage));
    }
    return result;
}

}  // namespace mso::search
