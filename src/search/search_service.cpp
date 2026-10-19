#include "mso/search/search_service.hpp"

#include <picosha2.h>

#include <algorithm>
#include <format>
#include <limits>
#include <map>

#include <nlohmann/json.hpp>

#include "mso/domain/json_fields.hpp"
#include "mso/domain/taxonomy.hpp"
#include "mso/search/detail/predicate_support.hpp"
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

namespace {

/// Version of the cursor token layout; version 2 added the library set to the fingerprint,
/// version 3 replaced the relevance offset with per-library ranks consumed, version 4 added the
/// per-library hydration retries.
constexpr int kCursorVersion = 4;

/// How many times the rows of a library whose hydration failed are offered again on the next
/// page before they are skipped. One retry absorbs a transient failure (a busy reader); a
/// persistent one (a damaged page) then costs one more page instead of paging forever.
constexpr int kHydrationRetries = 1;

/// SQL expression for each sort field; text sorts compare normalized keys.
std::string_view sort_expression(SortField field) {
    switch (field) {
        case SortField::Name:
        case SortField::Relevance:
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
    }
    return "s.name_key";
}

/// True for sorts whose key is text (compared as normalized strings); others are integers.
bool is_text_sort(SortField field) {
    return field == SortField::Name || field == SortField::Format;
}

/// True for errors caused by the request itself, which fail the whole call; anything else is a
/// failure of one library, reported in its coverage.
bool is_request_error(const Error& error) {
    return error.code == ErrorCode::ParseError || error.code == ErrorCode::InvalidArgument ||
           error.code == ErrorCode::StaleCursor;
}

/// Records a library failure in its coverage, or returns the error when it is a request error.
std::optional<Error> record_library_error(LibraryCoverage& coverage, Error error) {
    if (is_request_error(error)) return error;
    coverage.complete = false;
    coverage.error = std::move(error);
    return std::nullopt;
}

/// A matching sample of one library with the values that order it globally (metadata sorts).
struct OrderedKey {
    uuids::uuid library;
    std::int64_t sample_id = 0;
    int partition = 0;  ///< 0 = sort value present, 1 = null
    nlohmann::json sort_value;
};

/// Keyset position of one library inside a federated cursor.
struct LibraryPosition {
    int partition = 0;          ///< 0 = in the non-null values, 1 = in the nulls
    nlohmann::json value;       ///< last sort value (null, integer or string)
    std::int64_t last_id = 0;   ///< last sample id consumed
    bool started = false;       ///< false until a row of this library was consumed
    bool exhausted = false;     ///< no rows left after the position
    int hydration_retries = 0;  ///< rows after the position failed to hydrate this many pages in a row
};

/// A decoded continuation token.
struct DecodedCursor {
    std::map<std::string, LibraryPosition> positions;   ///< by library UUID text (metadata sorts)
    std::map<std::string, std::size_t> ranks_consumed;  ///< by library UUID text: ranked rows delivered (relevance)
    std::map<std::string, int> hydration_retries;       ///< by library UUID text, as in LibraryPosition (relevance)
};

/// Reads a retry count (0..kHydrationRetries) from a cursor; nullopt when malformed.
std::optional<int> decode_retries(const nlohmann::json& value) {
    const auto count = json_fields::integer_value(value);
    if (!count || *count < 0 || *count > kHydrationRetries) return std::nullopt;
    return static_cast<int>(*count);
}

/// Identifies the result set a cursor belongs to: query, scope, order and the set of libraries.
std::string query_fingerprint(const SearchRequest& request, std::span<catalog::Library* const> libraries) {
    const auto optional_json = [](const auto& value) { return value ? nlohmann::json(*value) : nlohmann::json(); };
    const nlohmann::json scope = {{"root", optional_json(request.scope.root_id)},
                                  {"folder", optional_json(request.scope.folder)},
                                  {"collection", optional_json(request.scope.collection_id)},
                                  {"tag", optional_json(request.scope.tag_id)},
                                  {"category", optional_json(request.scope.category_id)},
                                  {"uncategorized", request.scope.uncategorized}};
    std::vector<std::string> library_set;
    for (const auto* library : libraries)
        library_set.push_back(uuids::to_string(library->uuid()));
    std::ranges::sort(library_set);
    return picosha2::hash256_hex_string(json_fields::dump_json(nlohmann::json{{"text", request.text},
                                                                              {"scope", scope},
                                                                              {"sort", to_string(request.sort)},
                                                                              {"descending", request.descending},
                                                                              {"libraries", library_set}}));
}

/// Reads a cursor without ever throwing: any malformed or foreign token is STALE_CURSOR.
Result<DecodedCursor> decode_cursor(const std::optional<Cursor>& cursor, const std::string& fingerprint) {
    DecodedCursor decoded;
    if (!cursor) return decoded;
    const auto stale = [] { return fail(ErrorCode::StaleCursor, "the result set changed; refresh the view"); };
    const auto document = json_fields::parse_json(cursor->token);
    if (json_fields::integer_field(document, "v") != kCursorVersion ||
        json_fields::string_field(document, "q") != fingerprint)
        return stale();
    if (const auto* ranked = json_fields::member_field(document, "ranked")) {
        if (!ranked->is_object()) return stale();
        for (const auto& [library, consumed] : ranked->items()) {
            const auto count = json_fields::integer_value(consumed);
            if (!count || *count < 0) return stale();
            decoded.ranks_consumed.emplace(library, static_cast<std::size_t>(*count));
        }
    }
    if (const auto* retries = json_fields::member_field(document, "retry")) {
        if (!retries->is_object()) return stale();
        for (const auto& [library, count] : retries->items()) {
            const auto retried = decode_retries(count);
            if (!retried) return stale();
            decoded.hydration_retries.emplace(library, *retried);
        }
    }
    const auto* libraries = json_fields::member_field(document, "libraries");
    if (libraries == nullptr) return decoded;
    if (!libraries->is_object()) return stale();
    for (const auto& [library, state] : libraries->items()) {
        const auto partition = json_fields::integer_field(state, "p");
        const auto* value = json_fields::member_field(state, "v");
        const auto last_id = json_fields::integer_field(state, "id");
        const auto started = json_fields::boolean_field(state, "started");
        const auto exhausted = json_fields::boolean_field(state, "done");
        const auto* retries = json_fields::member_field(state, "retry");
        const auto retried = retries != nullptr ? decode_retries(*retries) : std::nullopt;
        if (!partition || (*partition != 0 && *partition != 1) || value == nullptr ||
            !(value->is_null() || value->is_number_integer() || value->is_string()) || !last_id || !started ||
            !exhausted || !retried)
            return stale();
        LibraryPosition position;
        position.partition = static_cast<int>(*partition);
        position.value = *value;
        position.last_id = *last_id;
        position.started = *started;
        position.exhausted = *exhausted;
        position.hydration_retries = *retried;
        decoded.positions.emplace(library, std::move(position));
    }
    return decoded;
}

/// Compiles the navigation scope into SQL for one library (`revision` names it, see compile_query).
Result<SqlPredicate> compile_scope(const Scope& scope, sqlite::Connection& connection,
                                   const LibraryRevision& revision) {
    SqlPredicate out;
    std::vector<std::string> parts;
    if (scope.folder && !scope.root_id)
        return fail(ErrorCode::InvalidArgument, "a folder scope needs the root it belongs to");
    if (scope.root_id) {
        auto root = connection.prepare("SELECT case_sensitive FROM roots WHERE id=?1");
        if (!root) return std::unexpected(root.error());
        root->bind(1, *scope.root_id);
        auto has_row = root->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        const bool case_sensitive = *has_row && root->column_int64(0) != 0;
        root->reset();
        std::string folder_key = scope.folder ? path_key(*scope.folder, case_sensitive) : std::string();
        while (folder_key.ends_with('/'))
            folder_key.pop_back();
        // The primary location lies in the root (and below the folder), as for `folder:`.
        SqlPredicate condition;
        condition.sql = "fl.root_id=?";
        condition.parameters.emplace_back(*scope.root_id);
        if (!folder_key.empty()) {
            const std::string prefix = folder_key + '/';
            condition.sql += " AND fl.path_key>=? AND fl.path_key<?";
            condition.parameters.emplace_back(prefix);
            condition.parameters.emplace_back(detail::prefix_upper_bound(prefix));
        }
        auto filtered = primary_location_filter(connection, condition, revision);
        if (!filtered) return std::unexpected(filtered.error());
        parts.push_back(std::move(filtered->sql));
        out.parameters.insert(out.parameters.end(), filtered->parameters.begin(), filtered->parameters.end());
    }
    if (scope.collection_id) {
        auto collection = compile_collection(*scope.collection_id, connection, revision);
        if (!collection) return std::unexpected(collection.error());
        parts.push_back('(' + collection->sql + ')');
        out.parameters.insert(out.parameters.end(), collection->parameters.begin(), collection->parameters.end());
    }
    const auto subtree = [](std::string_view table) {
        return std::format(
            "WITH RECURSIVE subtree(id,depth) AS (SELECT ?,0 UNION ALL SELECT t.id,subtree.depth+1 "
            "FROM {} t JOIN subtree ON t.parent_id=subtree.id WHERE subtree.depth<{}) SELECT id FROM subtree",
            table, kMaxTaxonomyDepth);
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

/// The query and scope of a request compiled for one library.
struct LibraryFilter {
    SqlPredicate query;  ///< the query text (compile_query)
    SqlPredicate scope;  ///< the navigation scope (compile_scope)
};

/// Compiles the query and the scope of a request for `library`, on one of its read connections.
Result<LibraryFilter> compile_filter(const Query& query, const Scope& scope, catalog::Library& library,
                                     sqlite::Connection& connection) {
    const LibraryRevision revision{library.uuid(), library.identity().catalog_revision};
    auto compiled_query = compile_query(query, connection, revision);
    if (!compiled_query) return std::unexpected(compiled_query.error());
    auto compiled_scope = compile_scope(scope, connection, revision);
    if (!compiled_scope) return std::unexpected(compiled_scope.error());
    return LibraryFilter{std::move(*compiled_query), std::move(*compiled_scope)};
}

/// The sort key of the current row (column 1) as JSON (null, integer or string).
nlohmann::json sort_value_json(const sqlite::PreparedStatement& statement, SortField field) {
    if (statement.column_is_null(1)) return nlohmann::json();
    if (is_text_sort(field)) return nlohmann::json(statement.column_text(1));
    return nlohmann::json(statement.column_int64(1));
}

/// The ids and sort keys of up to `limit` matches of one library after its keyset position,
/// across the non-null and null partitions (no hydration).
Result<std::vector<OrderedKey>> fetch_library_keys(catalog::Library& library, const Query& query,
                                                   const SearchRequest& request, const LibraryPosition& position,
                                                   std::size_t limit) {
    auto reader = library.acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto& connection = reader->connection();
    auto filter = compile_filter(query, request.scope, library, connection);
    if (!filter) return std::unexpected(filter.error());

    const std::string expression(sort_expression(request.sort));
    const std::string_view direction = request.descending ? "DESC" : "ASC";
    const std::string_view after = request.descending ? "<" : ">";
    std::vector<OrderedKey> keys;
    for (int partition = position.partition; partition <= 1 && keys.size() < limit; ++partition) {
        const bool resume = position.started && partition == position.partition;
        std::string keyset;
        if (partition == 0) {
            keyset = std::format("{} IS NOT NULL", expression);
            if (resume) keyset += std::format(" AND ({0} {1} ? OR ({0} = ? AND s.id {1} ?))", expression, after);
        } else {
            keyset = std::format("{} IS NULL", expression);
            if (resume) keyset += std::format(" AND s.id {} ?", after);
        }
        const std::string sql = std::format(
            "SELECT s.id, {0} FROM samples s LEFT JOIN effective_metadata e ON e.sample_id=s.id "
            "WHERE s.deleted=0 AND ({1}) AND ({2}) AND {3} ORDER BY {0} {4}, s.id {4} LIMIT ?",
            expression, filter->query.sql, filter->scope.sql, keyset, direction);
        auto statement = connection.prepare(sql);
        if (!statement) return std::unexpected(statement.error());
        int next = bind_parameters(*statement, filter->scope.parameters,
                                   bind_parameters(*statement, filter->query.parameters, 1));
        if (resume && partition == 0) {
            const nlohmann::json& value = position.value;
            for (int repeat = 0; repeat < 2; ++repeat) {
                if (value.is_number_integer())
                    statement->bind(next++, value.get<std::int64_t>());
                else
                    statement->bind(next++, value.is_string() ? value.get_ref<const std::string&>() : std::string());
            }
            statement->bind(next++, position.last_id);
        } else if (resume) {
            statement->bind(next++, position.last_id);
        }
        statement->bind(next, static_cast<std::int64_t>(limit - keys.size()));
        while (true) {
            auto has_row = statement->next_row();
            if (!has_row) return std::unexpected(has_row.error());
            if (!*has_row) break;
            keys.push_back(
                {library.uuid(), statement->column_int64(0), partition, sort_value_json(*statement, request.sort)});
        }
    }
    return keys;
}

/// Global order across libraries: partition, value (by direction), library UUID, id.
bool comes_before(const OrderedKey& left, const OrderedKey& right, bool descending) {
    if (left.partition != right.partition) return left.partition < right.partition;
    if (left.sort_value != right.sort_value)
        return descending ? right.sort_value < left.sort_value : left.sort_value < right.sort_value;
    if (left.library != right.library) return left.library < right.library;
    return descending ? left.sample_id > right.sample_id : left.sample_id < right.sample_id;
}

/// A sample to hydrate, identified by library and id.
struct PageEntry {
    uuids::uuid library;
    std::int64_t sample_id = 0;
};

/// Hydrates exactly the rows of one page (one load_rows call per library) in page order. A
/// library whose rows cannot be loaded is marked incomplete in `coverage` and its rows are left
/// out; the set of such libraries is returned.
std::vector<uuids::uuid> hydrate_page(std::span<catalog::Library* const> libraries,
                                      const std::vector<PageEntry>& entries, std::vector<LibraryCoverage>& coverage,
                                      std::vector<SampleRow>& rows) {
    std::map<uuids::uuid, std::map<std::int64_t, SampleRow>> loaded;
    std::vector<uuids::uuid> failed;
    for (auto* library : libraries) {
        std::vector<std::int64_t> ids;
        for (const auto& entry : entries)
            if (entry.library == library->uuid()) ids.push_back(entry.sample_id);
        if (ids.empty()) continue;
        auto hydrated = load_rows(*library, ids);
        if (!hydrated) {
            failed.push_back(library->uuid());
            for (auto& entry : coverage)
                if (entry.library == library->uuid()) {
                    entry.complete = false;
                    entry.error = hydrated.error();
                }
            continue;
        }
        auto& by_id = loaded[library->uuid()];
        for (auto& row : *hydrated)
            by_id.emplace(row.sample_id, std::move(row));
    }
    for (const auto& entry : entries) {
        auto library = loaded.find(entry.library);
        if (library == loaded.end()) continue;
        auto row = library->second.find(entry.sample_id);
        if (row != library->second.end()) rows.push_back(std::move(row->second));  // deleted meanwhile: skipped
    }
    return failed;
}

/// The page size actually served: `requested` clamped to 1..SearchService::kMaxPageSize.
std::size_t clamp_page_size(std::size_t requested) {
    return std::clamp<std::size_t>(requested, 1, SearchService::kMaxPageSize);
}

/// One relevance candidate of one library.
struct RankedKey {
    uuids::uuid library;
    std::int64_t sample_id = 0;
    bool text_match = false;  ///< matched a positive text term (has a BM25 score)
    double fused_score = 0;   ///< 1 / (k + rank within its library)
};

/// Global relevance order: text hits first, then fused score, library UUID and id. Within one
/// library it follows the library's own ranking (text hits are ranked first there too, and the
/// fused score falls with the rank), so per-library suffixes merge into the global continuation.
bool ranks_before(const RankedKey& left, const RankedKey& right) {
    if (left.text_match != right.text_match) return left.text_match;
    if (left.fused_score != right.fused_score) return left.fused_score > right.fused_score;
    if (left.library != right.library) return left.library < right.library;
    return left.sample_id < right.sample_id;
}

/// The libraries whose rows failed to hydrate on this page and are offered again on the next page:
/// those retried fewer than kHydrationRetries times so far (`retries_so_far(library)`). The rows
/// of the other failed libraries are skipped.
template <class RetriesSoFar>
std::vector<uuids::uuid> libraries_to_retry(const std::vector<uuids::uuid>& failed, RetriesSoFar&& retries_so_far) {
    std::vector<uuids::uuid> retried;
    for (const auto& library : failed)
        if (retries_so_far(library) < kHydrationRetries) retried.push_back(library);
    return retried;
}

/// Relevance sort: per library, the top kRelevanceDepth matches of the full filter, full-text hits
/// by BM25 first; libraries are fused by reciprocal rank, text hits ahead of rows that matched
/// only through a non-text branch (`kick OR tag:Drums`), which rank by name. The cursor records how
/// many ranked rows of each library were delivered, so rows of a library whose hydration failed
/// are retried on the next page (as metadata sorts do), kHydrationRetries times, and then skipped.
Result<SearchPage> relevance_search(std::span<catalog::Library* const> libraries, const SearchRequest& request,
                                    const Query& query, const DecodedCursor& cursor, std::size_t page_size,
                                    const std::string& fingerprint) {
    std::string match;
    for (const auto& term : positive_text_terms(query))
        match += (match.empty() ? "" : " OR ") + fts_expression(term);
    // When every match necessarily hits a text term, the FTS index can drive the query.
    const bool text_driven = requires_text_match(query);

    SearchPage page;
    std::vector<RankedKey> fused;
    std::map<uuids::uuid, std::size_t> ranked_counts;  ///< ranked rows per library that answered
    for (auto* library : libraries) {
        const auto identity = library->identity();
        LibraryCoverage coverage{identity.uuid, identity.name, true, std::nullopt};
        auto ranked = [&]() -> Result<std::vector<RankedKey>> {
            auto reader = library->acquire_reader();
            if (!reader) return std::unexpected(reader.error());
            auto filter = compile_filter(query, request.scope, *library, reader->connection());
            if (!filter) return std::unexpected(filter.error());
            constexpr std::string_view kRanked =
                "(SELECT rowid AS id, bm25(sample_fts) AS score FROM sample_fts WHERE sample_fts MATCH ?)";
            const std::string sql =
                text_driven ? std::format(
                                  "SELECT s.id, ranked.score FROM {0} ranked JOIN samples s ON s.id=ranked.id "
                                  "LEFT JOIN effective_metadata e ON e.sample_id=s.id WHERE s.deleted=0 AND ({1}) "
                                  "AND ({2}) ORDER BY ranked.score, s.id LIMIT ?",
                                  kRanked, filter->query.sql, filter->scope.sql)
                            : std::format(
                                  "SELECT s.id, ranked.score FROM samples s LEFT JOIN effective_metadata e "
                                  "ON e.sample_id=s.id LEFT JOIN {0} ranked ON ranked.id=s.id WHERE s.deleted=0 "
                                  "AND ({1}) AND ({2}) ORDER BY ranked.score IS NULL, ranked.score, s.name_key, s.id "
                                  "LIMIT ?",
                                  kRanked, filter->query.sql, filter->scope.sql);
            auto statement = reader->connection().prepare(sql);
            if (!statement) return std::unexpected(statement.error());
            statement->bind(1, match);
            const int next = bind_parameters(*statement, filter->scope.parameters,
                                             bind_parameters(*statement, filter->query.parameters, 2));
            statement->bind(next, static_cast<std::int64_t>(SearchService::kRelevanceDepth + 1));
            std::vector<RankedKey> keys;
            while (true) {
                auto has_row = statement->next_row();
                if (!has_row) return std::unexpected(has_row.error());
                if (!*has_row) break;
                const double rank = static_cast<double>(keys.size() + 1);
                keys.push_back({identity.uuid, statement->column_int64(0), !statement->column_is_null(1),
                                1.0 / (SearchService::kReciprocalRankConstant + rank)});
            }
            return keys;
        }();
        if (!ranked) {
            if (auto request_error = record_library_error(coverage, ranked.error()))
                return std::unexpected(*request_error);
        } else {
            if (ranked->size() > SearchService::kRelevanceDepth) {
                page.relevance_window_limited = true;
                ranked->resize(SearchService::kRelevanceDepth);
            }
            // Only this library's rows not delivered yet; its ranking order is the global one.
            const auto consumed = cursor.ranks_consumed.find(uuids::to_string(identity.uuid));
            const std::size_t skipped =
                std::min(consumed != cursor.ranks_consumed.end() ? consumed->second : 0, ranked->size());
            ranked_counts[identity.uuid] = ranked->size();
            fused.insert(fused.end(), ranked->begin() + static_cast<std::ptrdiff_t>(skipped), ranked->end());
        }
        page.coverage.push_back(std::move(coverage));
    }
    std::ranges::stable_sort(fused, ranks_before);
    if (fused.size() > page_size) fused.resize(page_size);

    std::vector<PageEntry> entries;
    std::map<std::pair<uuids::uuid, std::int64_t>, double> scores;
    for (const auto& key : fused) {
        entries.push_back({key.library, key.sample_id});
        scores.emplace(std::pair{key.library, key.sample_id}, key.text_match ? key.fused_score : 0.0);
    }
    const auto failed = hydrate_page(libraries, entries, page.coverage, page.rows);
    for (auto& row : page.rows)
        row.relevance = scores[{row.library, row.sample_id}];

    // Advance each library by the rows taken from it. A library whose rows could not be hydrated
    // keeps its count so the next page retries them; failing again skips them.
    std::map<std::string, int> next_retries = cursor.hydration_retries;
    const auto retried = libraries_to_retry(failed, [&](const uuids::uuid& library) {
        const auto found = next_retries.find(uuids::to_string(library));
        return found != next_retries.end() ? found->second : 0;
    });
    std::map<std::string, std::size_t> next_consumed = cursor.ranks_consumed;
    for (const auto& key : fused) {
        if (std::ranges::contains(retried, key.library)) continue;
        const auto library = uuids::to_string(key.library);
        ++next_consumed[library];
        next_retries.erase(library);  // delivered, or skipped after its retries
    }
    for (const auto& library : retried)
        ++next_retries[uuids::to_string(library)];
    bool more = false;
    for (const auto& [library, count] : ranked_counts) {
        const auto found = next_consumed.find(uuids::to_string(library));
        more = more || (found != next_consumed.end() ? found->second : 0) < count;
    }
    if (more) {
        nlohmann::json ranked_state = nlohmann::json::object();
        for (const auto& [library, consumed] : next_consumed)
            ranked_state[library] = consumed;
        nlohmann::json token{{"v", kCursorVersion}, {"q", fingerprint}, {"ranked", std::move(ranked_state)}};
        if (!next_retries.empty()) token["retry"] = next_retries;
        page.next = Cursor{json_fields::dump_json(token)};
    }
    return page;
}

/// Metadata sort: a page from each library after its own keyset position, k-way merged.
Result<SearchPage> metadata_search(std::span<catalog::Library* const> libraries, const SearchRequest& request,
                                   const Query& query, const DecodedCursor& cursor, std::size_t page_size,
                                   const std::string& fingerprint) {
    SearchRequest effective = request;
    if (effective.sort == SortField::Relevance) effective.sort = SortField::Name;
    SearchPage page;
    std::vector<OrderedKey> merged;
    std::map<uuids::uuid, std::size_t> fetched_counts;
    for (auto* library : libraries) {
        const auto identity = library->identity();
        LibraryCoverage coverage{identity.uuid, identity.name, true, std::nullopt};
        const auto key = uuids::to_string(identity.uuid);
        const auto stored = cursor.positions.find(key);
        const LibraryPosition position = stored != cursor.positions.end() ? stored->second : LibraryPosition{};
        if (!position.exhausted) {
            auto keys = fetch_library_keys(*library, query, effective, position, page_size);
            if (!keys) {
                if (auto request_error = record_library_error(coverage, keys.error()))
                    return std::unexpected(*request_error);
            } else {
                fetched_counts[identity.uuid] = keys->size();
                merged.insert(merged.end(), std::make_move_iterator(keys->begin()),
                              std::make_move_iterator(keys->end()));
            }
        }
        page.coverage.push_back(std::move(coverage));
    }
    std::ranges::stable_sort(merged, [&](const OrderedKey& left, const OrderedKey& right) {
        return comes_before(left, right, effective.descending);
    });
    if (merged.size() > page_size) merged.resize(page_size);

    std::vector<PageEntry> entries;
    for (const auto& key : merged)
        entries.push_back({key.library, key.sample_id});
    const auto failed = hydrate_page(libraries, entries, page.coverage, page.rows);

    // New per-library positions: the last key consumed from each; untouched libraries keep theirs.
    // A library whose rows could not be hydrated keeps its position so the next page retries them;
    // failing again at that position skips them, so a persistent failure cannot page forever.
    std::map<std::string, LibraryPosition> next_positions = cursor.positions;
    const auto retried = libraries_to_retry(failed, [&](const uuids::uuid& library) {
        const auto found = next_positions.find(uuids::to_string(library));
        return found != next_positions.end() ? found->second.hydration_retries : 0;
    });
    std::map<uuids::uuid, std::size_t> consumed;
    for (const auto& key : merged) {
        if (std::ranges::contains(retried, key.library)) continue;
        ++consumed[key.library];
        auto& position = next_positions[uuids::to_string(key.library)];
        position.partition = key.partition;
        position.value = key.sort_value;
        position.last_id = key.sample_id;
        position.started = true;
        position.hydration_retries = 0;  // delivered, or skipped after its retries
    }
    for (const auto& library : retried)
        ++next_positions[uuids::to_string(library)].hydration_retries;
    bool more = false;
    for (const auto& [library, fetched] : fetched_counts) {
        const bool library_has_more =
            std::ranges::contains(retried, library) || fetched == page_size || consumed[library] < fetched;
        next_positions[uuids::to_string(library)].exhausted = !library_has_more;
        more = more || library_has_more;
    }
    if (more) {
        nlohmann::json state = nlohmann::json::object();
        for (const auto& [key, position] : next_positions)
            state[key] = {{"p", position.partition},    {"v", position.value},
                          {"id", position.last_id},     {"started", position.started},
                          {"done", position.exhausted}, {"retry", position.hydration_retries}};
        page.next = Cursor{
            json_fields::dump_json(nlohmann::json{{"v", kCursorVersion}, {"q", fingerprint}, {"libraries", state}})};
    }
    return page;
}

/// Checks every multi-segment `tag:`/`category:` value of a query the way the catalog will
/// (split_taxonomy_path), so text that can never compile is refused before it is stored as a
/// smart collection: INVALID_ARGUMENT, the same error a search with that text reports.
Result<void> validate_taxonomy_paths(const QueryNode& node) {
    for (const auto& child : node.children)
        if (auto valid = validate_taxonomy_paths(*child); !valid) return valid;
    if (node.kind != QueryNode::Kind::Predicate) return {};
    const auto& predicate = node.predicate;
    if (predicate.field != Field::Tag && predicate.field != Field::Category) return {};
    if (predicate.text.find('/') == std::string::npos) return {};
    if (auto segments = split_taxonomy_path(predicate.text); !segments)
        return fail(ErrorCode::InvalidArgument, std::format("{}:{} is not a valid path: {}", to_string(predicate.field),
                                                            predicate.text, segments.error().safe_detail));
    return {};
}

}  // namespace

// ---------------------------------------------------------------- free functions

Result<std::vector<std::int64_t>> matching_ids(catalog::Library& library, const SearchRequest& request) {
    return matching_ids(library, request, std::numeric_limits<std::size_t>::max());
}

Result<std::vector<std::int64_t>> matching_ids(catalog::Library& library, const SearchRequest& request,
                                               std::size_t maximum_ids) {
    auto query = parse_query(request.text);
    if (!query) return std::unexpected(query.error());
    auto reader = library.acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto filter = compile_filter(*query, request.scope, library, reader->connection());
    if (!filter) return std::unexpected(filter.error());
    const bool bounded = maximum_ids != std::numeric_limits<std::size_t>::max();
    // `+s.deleted`: on the (deleted, ...) sort indexes SQLite would read every live sample and sort
    // the matches; on the rowid it is driven by the full-text hits or walks the table in id order.
    auto statement = reader->connection().prepare(
        std::format("SELECT s.id FROM samples s LEFT JOIN effective_metadata e ON e.sample_id=s.id "
                    "WHERE +s.deleted=0 AND ({}) AND ({}) ORDER BY s.id{}",
                    filter->query.sql, filter->scope.sql, bounded ? " LIMIT ?" : ""));
    if (!statement) return std::unexpected(statement.error());
    const int next =
        bind_parameters(*statement, filter->scope.parameters, bind_parameters(*statement, filter->query.parameters, 1));
    if (bounded) {
        // One row more than allowed tells "exactly the limit" from "too many".
        statement->bind(next,
                        static_cast<std::int64_t>(
                            std::min<std::size_t>(maximum_ids, std::numeric_limits<std::int64_t>::max() - 1) + 1));
    }
    std::vector<std::int64_t> ids;
    while (true) {
        auto has_row = statement->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) break;
        if (ids.size() == maximum_ids)
            return fail(ErrorCode::ResourceLimit, std::format("more than {} samples match", maximum_ids));
        ids.push_back(statement->column_int64(0));
    }
    return ids;
}

// ---------------------------------------------------------------- SearchService

Result<void> SearchService::validate(std::string_view text) const {
    auto parsed = parse_query(text);
    if (!parsed) return std::unexpected(parsed.error());
    if (parsed->root) return validate_taxonomy_paths(*parsed->root);
    return {};
}

Result<SearchPage> SearchService::search(std::span<catalog::Library* const> libraries,
                                         const SearchRequest& request) const {
    auto query = parse_query(request.text);
    if (!query) return std::unexpected(query.error());
    const std::size_t page_size = clamp_page_size(request.page_size);
    const std::string fingerprint = query_fingerprint(request, libraries);
    auto cursor = decode_cursor(request.cursor, fingerprint);
    if (!cursor) return std::unexpected(cursor.error());
    if (request.sort == SortField::Relevance && !positive_text_terms(*query).empty())
        return relevance_search(libraries, request, *query, *cursor, page_size, fingerprint);
    return metadata_search(libraries, request, *query, *cursor, page_size, fingerprint);
}

Result<CountResult> SearchService::count(std::span<catalog::Library* const> libraries,
                                         const SearchRequest& request) const {
    auto query = parse_query(request.text);
    if (!query) return std::unexpected(query.error());
    CountResult result;
    for (auto* library : libraries) {
        const auto identity = library->identity();
        LibraryCoverage coverage{identity.uuid, identity.name, true, std::nullopt};
        auto counted = [&]() -> Result<std::int64_t> {
            auto reader = library->acquire_reader();
            if (!reader) return std::unexpected(reader.error());
            auto filter = compile_filter(*query, request.scope, *library, reader->connection());
            if (!filter) return std::unexpected(filter.error());
            auto statement = reader->connection().prepare(
                std::format("SELECT count(*) FROM samples s LEFT JOIN effective_metadata e ON e.sample_id=s.id "
                            "WHERE s.deleted=0 AND ({}) AND ({})",
                            filter->query.sql, filter->scope.sql));
            if (!statement) return std::unexpected(statement.error());
            bind_parameters(*statement, filter->scope.parameters,
                            bind_parameters(*statement, filter->query.parameters, 1));
            auto has_row = statement->next_row();
            if (!has_row) return std::unexpected(has_row.error());
            return *has_row ? statement->column_int64(0) : 0;
        }();
        if (counted)
            result.total += *counted;
        else if (auto request_error = record_library_error(coverage, counted.error()))
            return std::unexpected(*request_error);
        result.coverage.push_back(std::move(coverage));
    }
    return result;
}

}  // namespace mso::search
