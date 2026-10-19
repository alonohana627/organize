#include "mso/search/query_compiler.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <optional>

#include "mso/catalog/taxonomy_lookup.hpp"
#include "mso/domain/taxonomy.hpp"
#include "mso/search/detail/predicate_support.hpp"

namespace mso::search {

namespace {

/// Deepest chain of smart collections referring to smart collections.
constexpr int kMaxSmartCollectionNesting = 4;

constexpr std::string_view kAvailableSql =
    "EXISTS(SELECT 1 FROM locations al JOIN roots ar ON ar.id=al.root_id WHERE al.sample_id=s.id "
    "AND al.state='present' AND ar.availability<>'offline' AND ar.deleted=0)";

/// Escapes LIKE wildcards so user text matches literally (used with ESCAPE '\').
std::string escape_like(std::string_view text) {
    std::string escaped;
    for (const char character : text) {
        if (character == '%' || character == '_' || character == '\\') escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

/// The facts of a root that `folder:` needs.
struct RootFacts {
    std::int64_t id = 0;          ///< root id
    std::string name;             ///< display name
    bool case_sensitive = false;  ///< whether its path keys keep case
};

/// The facts of a live collection that `collection:` and the collection scope need.
struct CollectionFacts {
    std::int64_t id = 0;       ///< collection id
    std::string name;          ///< display name (for error messages)
    std::string folded_name;   ///< fold_key(name), what `collection:<name>` compares
    bool smart = false;        ///< smart (stored query) rather than manual membership
    std::string stored_query;  ///< smart collections: the stored query_json
};

/// Counts one more level of smart-collection nesting for as long as it lives.
class NestingLevel {
public:
    /// Increments `level`; the destructor decrements it again.
    explicit NestingLevel(int& level) : level_(level) { ++level_; }
    ~NestingLevel() { --level_; }
    NestingLevel(const NestingLevel&) = delete;
    NestingLevel& operator=(const NestingLevel&) = delete;

private:
    int& level_;
};

/// Compiles one query, and every smart collection it references, for one library. One instance
/// serves a whole expansion, so roots, collections, folder fragments and tag redirects are read or
/// built once however many smart collections repeat them, and every nested query shares the
/// kMaxCompiledNodes budget.
class QueryCompiler {
public:
    /// Compiles against the read connection `connection` (one snapshot).
    explicit QueryCompiler(sqlite::Connection& connection) : connection_(connection) {}

    /// Compiles a whole query; the empty query compiles to "1".
    Result<SqlPredicate> compile(const Query& query) {
        SqlPredicate predicate;
        if (query.empty()) return predicate;
        predicate.sql.clear();
        if (auto compiled = compile_node(*query.root, predicate, false); !compiled)
            return std::unexpected(compiled.error());
        return predicate;
    }

    /// Compiles collection `id` on its own (the collection scope).
    Result<SqlPredicate> compile_collection(std::int64_t id) {
        SqlPredicate predicate;
        predicate.sql.clear();
        if (auto compiled = append_collection(id, predicate); !compiled) return std::unexpected(compiled.error());
        return predicate;
    }

private:
    /// Appends collection `id`: manual membership, or the stored query of a smart collection
    /// (compiled inline with this compiler's caches and node budget). A missing or deleted
    /// collection matches nothing. Problems of the stored query are this library's problems
    /// (DB_CORRUPT, RESOURCE_LIMIT), never request errors (see compile_query).
    Result<void> append_collection(std::int64_t id, SqlPredicate& out) {
        auto all = collections();
        if (!all) return std::unexpected(all.error());
        const auto found = std::ranges::find(**all, id, &CollectionFacts::id);
        if (found == (*all)->end()) {
            out.sql += "0";
            return {};
        }
        if (!found->smart) {
            out.sql += "EXISTS(SELECT 1 FROM collection_items ci WHERE ci.collection_id=? AND ci.sample_id=s.id)";
            out.parameters.emplace_back(id);
            return {};
        }
        if (nesting_ >= kMaxSmartCollectionNesting)
            return fail(ErrorCode::ResourceLimit,
                        std::format("smart collection '{}' nests smart collections too deeply", found->name));
        const auto text = detail::smart_collection_text(found->stored_query);
        auto parsed = text ? parse_query(*text) : Result<Query>(fail(ErrorCode::ParseError, "no query text"));
        if (!parsed)
            return fail(ErrorCode::DbCorrupt, std::format("smart collection '{}' has an invalid query: {}", found->name,
                                                          parsed.error().safe_detail));
        if (parsed->empty()) {
            out.sql += "1";
            return {};
        }
        const NestingLevel level(nesting_);
        out.sql += '(';
        if (auto compiled = compile_node(*parsed->root, out, false); !compiled) {
            const Error& error = compiled.error();
            if (error.code == ErrorCode::InvalidArgument || error.code == ErrorCode::ParseError)
                return fail(ErrorCode::DbCorrupt, std::format("smart collection '{}' has an invalid query: {}",
                                                              found->name, error.safe_detail));
            return compiled;
        }
        out.sql += ')';
        return {};
    }

    /// Appends the SQL of one node. Predicates under a NOT are wrapped in COALESCE(…,0) so a
    /// predicate on an unknown (NULL) value is false and its negation true (query.hpp). Outside a
    /// NOT a NULL already rejects the row exactly like false, so those stay index-friendly.
    Result<void> compile_node(const QueryNode& node, SqlPredicate& out, bool negated) {
        // Only smart collections can push an expansion past the budget: typed text has at most
        // kMaxQueryNodes (256) nodes.
        if (++compiled_nodes_ > kMaxCompiledNodes)
            return fail(ErrorCode::ResourceLimit, "the query and its smart collections expand to too many terms");
        switch (node.kind) {
            case QueryNode::Kind::And:
            case QueryNode::Kind::Or: {
                const std::string_view joiner = node.kind == QueryNode::Kind::And ? " AND " : " OR ";
                out.sql += '(';
                for (std::size_t index = 0; index < node.children.size(); ++index) {
                    if (index) out.sql += joiner;
                    if (auto compiled = compile_node(*node.children[index], out, negated); !compiled) return compiled;
                }
                out.sql += ')';
                return {};
            }
            case QueryNode::Kind::Not:
                out.sql += "NOT (";
                if (auto compiled = compile_node(*node.children.front(), out, true); !compiled) return compiled;
                out.sql += ')';
                return {};
            case QueryNode::Kind::Text:  // never NULL: s.id is never NULL
                out.sql += "s.id IN (SELECT rowid FROM sample_fts WHERE sample_fts MATCH ?)";
                out.parameters.emplace_back(fts_expression(node.text));
                return {};
            case QueryNode::Kind::Predicate: {
                const std::size_t start = out.sql.size();
                if (auto compiled = compile_predicate(node.predicate, out); !compiled) return compiled;
                if (negated) {
                    out.sql.insert(start, "COALESCE((");
                    out.sql += "),0)";
                }
                return {};
            }
        }
        return fail(ErrorCode::Internal, "unknown query node");
    }

    /// Numeric bounds are already inclusive integers (see FieldPredicate).
    static void compile_numeric(std::string_view column, const FieldPredicate& predicate, SqlPredicate& out) {
        switch (predicate.comparison) {
            case Comparison::Equal:
            case Comparison::Between:
                out.sql += std::format("{} BETWEEN ? AND ?", column);
                out.parameters.emplace_back(predicate.low);
                out.parameters.emplace_back(predicate.high);
                return;
            case Comparison::Less:
            case Comparison::LessOrEqual:
                out.sql += std::format("{} <= ?", column);
                out.parameters.emplace_back(predicate.high);
                return;
            case Comparison::Greater:
            case Comparison::GreaterOrEqual:
                out.sql += std::format("{} >= ?", column);
                out.parameters.emplace_back(predicate.low);
                return;
        }
    }

    /// Appends one field predicate.
    Result<void> compile_predicate(const FieldPredicate& predicate, SqlPredicate& out) {
        switch (predicate.field) {
            case Field::Tag:
                return compile_taxonomy(predicate.text, catalog::TaxonomyKind::Tags, out);
            case Field::Category:
                return compile_taxonomy(predicate.text, catalog::TaxonomyKind::Categories, out);
            case Field::Folder:
                return compile_folder(predicate.text, out);
            case Field::Collection:
                return compile_collection_name(predicate.text, out);
            case Field::Bpm:
                compile_numeric("e.bpm_milli", predicate, out);
                return {};
            case Field::Duration:
                compile_numeric("e.duration_us", predicate, out);
                return {};
            case Field::Rating:
                compile_numeric("s.rating", predicate, out);
                return {};
            case Field::SampleRate:
                compile_numeric("e.sample_rate", predicate, out);
                return {};
            case Field::Channels:
                compile_numeric("e.channels", predicate, out);
                return {};
            case Field::BitDepth:
                compile_numeric("e.bit_depth", predicate, out);
                return {};
            case Field::Size:
                compile_numeric("e.byte_size", predicate, out);
                return {};
            case Field::Favorite:
                out.sql += "s.favorite = ?";
                out.parameters.emplace_back(std::int64_t{predicate.flag ? 1 : 0});
                return {};
            case Field::Available:
                out.sql += predicate.flag ? std::string(kAvailableSql) : std::format("NOT {}", kAvailableSql);
                return {};
            case Field::Analyzed:
                out.sql += predicate.flag ? "(COALESCE(e.analysis_flags,0) & ?) <> 0"
                                          : "(COALESCE(e.analysis_flags,0) & ?) = 0";
                out.parameters.emplace_back(predicate.low);
                return {};
            case Field::Format:
                out.sql += "e.format = ?";
                out.parameters.emplace_back(predicate.text);
                return {};
            case Field::Codec:  // LIKE: prefix match, ASCII case-insensitive
                out.sql += "e.codec LIKE ? ESCAPE '\\'";
                out.parameters.emplace_back(escape_like(predicate.text) + "%");
                return {};
            case Field::Key:
                if (!predicate.key) {
                    out.sql += "e.key_class IS NULL";
                    return {};
                }
                out.sql += "(e.key_class = ? AND e.key_mode = ?)";
                out.parameters.emplace_back(std::int64_t{predicate.key->pitch_class});
                out.parameters.emplace_back(std::string(to_string(predicate.key->mode)));
                return {};
            case Field::Name:
            case Field::Path:
            case Field::Notes:
                break;
        }
        return fail(ErrorCode::Internal, "field is not a predicate");
    }

    /// The live tags that merged tags named `name_key` redirect to: the end of each alias chain
    /// (bounded like the catalog's canonical_tag()), cached per name. Mirrors load_tag_aliases().
    Result<const std::vector<std::int64_t>*> redirected_tags(const std::string& name_key) {
        if (const auto cached = redirects_.find(name_key); cached != redirects_.end()) return &cached->second;
        auto statement = connection_.prepare(std::format(
            "WITH RECURSIVE redirect(id,hops) AS ("
            "SELECT a.canonical_tag_id, 1 FROM tag_aliases a JOIN tags t ON t.id=a.source_tag_id WHERE t.name_key=?1 "
            "UNION SELECT a.canonical_tag_id, redirect.hops+1 FROM redirect "
            "JOIN tag_aliases a ON a.source_tag_id=redirect.id WHERE redirect.hops<={0}) "
            "SELECT DISTINCT redirect.id FROM redirect JOIN tags canonical ON canonical.id=redirect.id "
            "WHERE canonical.deleted=0 AND redirect.id NOT IN (SELECT source_tag_id FROM tag_aliases) "
            "ORDER BY redirect.id",
            kMaxTaxonomyDepth));
        if (!statement) return std::unexpected(statement.error());
        statement->bind(1, name_key);
        std::vector<std::int64_t> ids;
        while (true) {
            auto has_row = statement->next_row();
            if (!has_row) return std::unexpected(has_row.error());
            if (!*has_row) break;
            ids.push_back(statement->column_int64(0));
        }
        return &redirects_.emplace(name_key, std::move(ids)).first->second;
    }

    /// tag:/category: match the whole subtree. A single-segment value matches every live node
    /// with that name at any depth ("tag:Kick" finds "Drums/Kick") and, for tags, every tag a
    /// merged tag of that name redirects to; a path resolves from the top (following merged tags
    /// to their canonical tag).
    Result<void> compile_taxonomy(const std::string& value, catalog::TaxonomyKind kind, SqlPredicate& out) {
        const bool tags = kind == catalog::TaxonomyKind::Tags;
        const std::string_view table = tags ? "tags" : "categories";
        std::string seed;
        if (value.find('/') == std::string::npos) {
            const std::string name_key = fold_key(value);
            seed = std::format("SELECT id FROM {} WHERE deleted=0 AND name_key=?", table);
            out.parameters.emplace_back(name_key);
            if (tags) {
                auto redirected = redirected_tags(name_key);
                if (!redirected) return std::unexpected(redirected.error());
                if (!(*redirected)->empty()) {
                    seed += " UNION SELECT value FROM json_each(?)";
                    out.parameters.emplace_back(detail::id_array_json(**redirected));
                }
            }
        } else {
            auto node = catalog::find_taxonomy_node(connection_, kind, value);
            if (!node) return std::unexpected(node.error());
            if (!*node) {
                out.sql += "0";
                return {};
            }
            seed = "SELECT ? AS id";  // named: an unnamed column would let `id` bind to the outer s.id
            out.parameters.emplace_back(**node);
        }
        const std::string subtree = std::format(
            "WITH RECURSIVE subtree(id,depth) AS (SELECT id,0 FROM ({0}) UNION ALL "
            "SELECT t.id,subtree.depth+1 FROM {1} t JOIN subtree ON t.parent_id=subtree.id WHERE subtree.depth<{2}) "
            "SELECT id FROM subtree",
            seed, table, kMaxTaxonomyDepth);
        // The seed parameters were pushed before this text; they are the only '?' in this fragment.
        out.sql +=
            tags ? std::format("EXISTS(SELECT 1 FROM sample_tags st WHERE st.sample_id=s.id AND st.tag_id IN ({}))",
                               subtree)
                 : std::format("s.category_id IN ({})", subtree);
        return {};
    }

    /// The id, name and case sensitivity of every root, read once per compilation.
    Result<const std::vector<RootFacts>*> roots() {
        if (roots_) return &*roots_;
        auto statement = connection_.prepare("SELECT id, name, case_sensitive FROM roots ORDER BY id");
        if (!statement) return std::unexpected(statement.error());
        std::vector<RootFacts> facts;
        while (true) {
            auto has_row = statement->next_row();
            if (!has_row) return std::unexpected(has_row.error());
            if (!*has_row) break;
            facts.push_back({statement->column_int64(0), statement->column_text(1), statement->column_int64(2) != 0});
        }
        roots_ = std::move(facts);
        return &*roots_;
    }

    /// Every live collection in id order, read once per compilation.
    Result<const std::vector<CollectionFacts>*> collections() {
        if (collections_) return &*collections_;
        auto statement =
            connection_.prepare("SELECT id, name, kind, query_json FROM collections WHERE deleted=0 ORDER BY id");
        if (!statement) return std::unexpected(statement.error());
        std::vector<CollectionFacts> facts;
        while (true) {
            auto has_row = statement->next_row();
            if (!has_row) return std::unexpected(has_row.error());
            if (!*has_row) break;
            CollectionFacts collection;
            collection.id = statement->column_int64(0);
            collection.name = statement->column_text(1);
            collection.folded_name = fold_key(collection.name);
            collection.smart = statement->column_text(2) == "smart";
            collection.stored_query = statement->column_optional_text(3).value_or("");
            facts.push_back(std::move(collection));
        }
        collections_ = std::move(facts);
        return &*collections_;
    }

    /// folder:"Packs/Goa" matches a sample whose primary location lies under that folder of any
    /// root, under "<root name>/<folder>", or anywhere in a root of that name
    /// (detail::match_folder_in_root, shared with the in-memory matcher). The location ranges are
    /// path-key ranges of the live (root_id, path_key) index; primary_location_filter picks the
    /// index-driven or the streaming shape by selectivity. Fragments are cached per value.
    Result<void> compile_folder(const std::string& value, SqlPredicate& out) {
        if (const auto cached = folder_fragments_.find(value); cached != folder_fragments_.end()) {
            out.sql += cached->second.sql;
            out.parameters.insert(out.parameters.end(), cached->second.parameters.begin(),
                                  cached->second.parameters.end());
            return {};
        }
        auto all_roots = roots();
        if (!all_roots) return std::unexpected(all_roots.error());
        SqlPredicate fragment;
        std::string alternatives;
        for (const auto& root : **all_roots) {
            const auto match = detail::match_folder_in_root(value, root.name, root.case_sensitive);
            if (!match.whole_root && match.prefixes.empty()) continue;
            if (!alternatives.empty()) alternatives += " OR ";
            alternatives += "(fl.root_id=?";
            fragment.parameters.emplace_back(root.id);
            if (!match.whole_root) {
                alternatives += " AND (";
                for (std::size_t index = 0; index < match.prefixes.size(); ++index) {
                    if (index) alternatives += " OR ";
                    alternatives += "(fl.path_key>=? AND fl.path_key<?)";
                    fragment.parameters.emplace_back(match.prefixes[index]);
                    fragment.parameters.emplace_back(detail::prefix_upper_bound(match.prefixes[index]));
                }
                alternatives += ')';
            }
            alternatives += ')';
        }
        if (alternatives.empty()) {
            fragment.sql = "0";
        } else {
            fragment.sql = std::move(alternatives);
            auto filtered = primary_location_filter(connection_, fragment);
            if (!filtered) return std::unexpected(filtered.error());
            fragment = std::move(*filtered);
        }
        out.sql += fragment.sql;
        out.parameters.insert(out.parameters.end(), fragment.parameters.begin(), fragment.parameters.end());
        folder_fragments_.emplace(value, std::move(fragment));
        return {};
    }

    /// collection:Name compares folded names (Unicode case-insensitive); the oldest live
    /// collection of that name wins.
    Result<void> compile_collection_name(const std::string& value, SqlPredicate& out) {
        auto all = collections();
        if (!all) return std::unexpected(all.error());
        const auto found = std::ranges::find(**all, fold_key(value), &CollectionFacts::folded_name);
        if (found == (*all)->end()) {
            out.sql += "0";
            return {};
        }
        return append_collection(found->id, out);
    }

    sqlite::Connection& connection_;
    int nesting_ = 0;                 ///< smart collections currently being expanded
    std::size_t compiled_nodes_ = 0;  ///< nodes compiled so far, shared by the whole expansion
    std::optional<std::vector<RootFacts>> roots_;
    std::optional<std::vector<CollectionFacts>> collections_;
    std::map<std::string, SqlPredicate> folder_fragments_;        ///< by folder: value
    std::map<std::string, std::vector<std::int64_t>> redirects_;  ///< by folded tag name
};

}  // namespace

Result<SqlPredicate> primary_location_filter(sqlite::Connection& connection, const SqlPredicate& location_condition) {
    // The largest sample id bounds the library size in O(log n) (ids are never reused).
    auto largest = connection.prepare("SELECT COALESCE(max(id),0) FROM samples");
    if (!largest) return std::unexpected(largest.error());
    auto has_largest = largest->next_row();
    if (!has_largest) return std::unexpected(has_largest.error());
    const std::int64_t library_size = *has_largest ? largest->column_int64(0) : 0;
    largest->reset();
    const std::int64_t threshold = std::max<std::int64_t>(1, library_size / kIndexDrivenLocationShare);

    // Candidate locations, counted index-only and never past the threshold.
    auto counted = connection.prepare(
        std::format("SELECT count(*) FROM (SELECT 1 FROM locations fl WHERE fl.state<>'retired' AND ({}) LIMIT ?)",
                    location_condition.sql));
    if (!counted) return std::unexpected(counted.error());
    counted->bind(bind_parameters(*counted, location_condition.parameters, 1), threshold);
    auto has_count = counted->next_row();
    if (!has_count) return std::unexpected(has_count.error());
    const std::int64_t candidates = *has_count ? counted->column_int64(0) : 0;
    counted->reset();

    SqlPredicate filter;
    filter.parameters = location_condition.parameters;
    if (candidates < threshold)
        filter.sql = std::format(
            "s.id IN (SELECT fl.sample_id FROM locations fl WHERE fl.state<>'retired' AND ({}) "
            "AND fl.id={})",
            location_condition.sql, detail::primary_location_id_sql("fl.sample_id"));
    else
        filter.sql = std::format("EXISTS(SELECT 1 FROM locations fl WHERE fl.id={} AND ({}))",
                                 detail::primary_location_id_sql("s.id"), location_condition.sql);
    return filter;
}

int bind_parameters(sqlite::PreparedStatement& statement, const std::vector<SqlValue>& parameters, int first) {
    int index = first;
    for (const auto& parameter : parameters) {
        std::visit([&](const auto& value) { statement.bind(index, value); }, parameter);
        ++index;
    }
    return index;
}

std::string fts_expression(const TextTerm& term) {
    std::string quoted = "\"";
    for (const char character : term.text) {
        if (character == '"') quoted.push_back('"');
        quoted.push_back(character);
    }
    quoted.push_back('"');
    if (term.prefix) quoted += " *";
    if (term.column) return std::format("{{{}}} : {}", to_string(*term.column), quoted);
    return quoted;
}

Result<SqlPredicate> compile_query(const Query& query, sqlite::Connection& connection) {
    return QueryCompiler(connection).compile(query);
}

Result<SqlPredicate> compile_collection(std::int64_t collection_id, sqlite::Connection& connection) {
    return QueryCompiler(connection).compile_collection(collection_id);
}

std::string_view availability_sql() {
    return kAvailableSql;
}

}  // namespace mso::search
