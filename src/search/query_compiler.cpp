#include "mso/search/query_compiler.hpp"

#include <format>

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
    std::int64_t id = 0;
    std::string name;
    bool case_sensitive = false;
};

/// Compiles one query (and, recursively, the smart collections it references) for one library.
class QueryCompiler {
public:
    /// `compiled_nodes` is shared by the whole expansion so nested smart collections share one budget.
    QueryCompiler(sqlite::Connection& connection, int nesting, std::size_t& compiled_nodes)
        : connection_(connection), nesting_(nesting), compiled_nodes_(compiled_nodes) {}

    /// Compiles a whole query; the empty query compiles to "1".
    Result<SqlPredicate> compile(const Query& query) {
        SqlPredicate predicate;
        if (query.empty()) return predicate;
        predicate.sql.clear();
        if (auto compiled = compile_node(*query.root, predicate, false); !compiled)
            return std::unexpected(compiled.error());
        return predicate;
    }

    /// Appends collection `id`: manual membership, or the stored query of a smart collection.
    Result<void> compile_collection_id(std::int64_t id, SqlPredicate& out) {
        auto lookup = connection_.prepare("SELECT name, kind, query_json FROM collections WHERE id=?1 AND deleted=0");
        if (!lookup) return std::unexpected(lookup.error());
        lookup->bind(1, id);
        auto has_row = lookup->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) {
            out.sql += "0";
            return {};
        }
        const std::string name = lookup->column_text(0);
        const std::string kind = lookup->column_text(1);
        const std::string stored_query = lookup->column_optional_text(2).value_or("");
        lookup->reset();
        if (kind == "manual") {
            out.sql += "EXISTS(SELECT 1 FROM collection_items ci WHERE ci.collection_id=? AND ci.sample_id=s.id)";
            out.parameters.emplace_back(id);
            return {};
        }
        if (nesting_ >= kMaxSmartCollectionNesting)
            return fail(ErrorCode::InvalidArgument, "smart collections are nested too deeply");
        const auto text = detail::smart_collection_text(stored_query);
        auto parsed = text ? parse_query(*text) : Result<Query>(fail(ErrorCode::ParseError));
        if (!parsed)
            return fail(ErrorCode::InvalidArgument, std::format("smart collection '{}' has an invalid query", name));
        auto compiled = QueryCompiler(connection_, nesting_ + 1, compiled_nodes_).compile(*parsed);
        if (!compiled) return std::unexpected(compiled.error());
        out.sql += '(' + compiled->sql + ')';
        out.parameters.insert(out.parameters.end(), compiled->parameters.begin(), compiled->parameters.end());
        return {};
    }

private:
    /// Appends the SQL of one node. Predicates under a NOT are wrapped in COALESCE(…,0) so a
    /// predicate on an unknown (NULL) value is false and its negation true (query.hpp). Outside a
    /// NOT a NULL already rejects the row exactly like false, so those stay index-friendly.
    Result<void> compile_node(const QueryNode& node, SqlPredicate& out, bool negated) {
        if (++compiled_nodes_ > kMaxCompiledNodes)
            return fail(ErrorCode::InvalidArgument, "the query and its smart collections expand to too many terms");
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

    /// tag:/category: match the whole subtree. A single-segment value matches every live node
    /// with that name at any depth ("tag:Kick" finds "Drums/Kick"); a path resolves from the top
    /// (following merged tags to their canonical tag).
    Result<void> compile_taxonomy(const std::string& value, catalog::TaxonomyKind kind, SqlPredicate& out) {
        const bool tags = kind == catalog::TaxonomyKind::Tags;
        const std::string_view table = tags ? "tags" : "categories";
        std::string seed;
        if (value.find('/') == std::string::npos) {
            seed = std::format("SELECT id FROM {} WHERE deleted=0 AND name_key=?", table);
            out.parameters.emplace_back(fold_key(value));
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
        // The seed parameter was pushed before this text; it is the only '?' in this fragment.
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

    /// folder:"Packs/Goa" matches a sample whose primary location lies under that folder of any
    /// root, under "<root name>/<folder>", or anywhere in a root of that name
    /// (detail::match_folder_in_root, shared with the in-memory matcher).
    Result<void> compile_folder(const std::string& value, SqlPredicate& out) {
        auto all_roots = roots();
        if (!all_roots) return std::unexpected(all_roots.error());
        std::string alternatives;
        for (const auto& root : **all_roots) {
            const auto match = detail::match_folder_in_root(value, root.name, root.case_sensitive);
            if (!match.whole_root && match.prefixes.empty()) continue;
            if (!alternatives.empty()) alternatives += " OR ";
            alternatives += "(fl.root_id=?";
            out.parameters.emplace_back(root.id);
            if (!match.whole_root) {
                alternatives += " AND (";
                for (std::size_t index = 0; index < match.prefixes.size(); ++index) {
                    if (index) alternatives += " OR ";
                    alternatives += "(fl.path_key>=? AND fl.path_key<?)";
                    out.parameters.emplace_back(match.prefixes[index]);
                    out.parameters.emplace_back(detail::prefix_upper_bound(match.prefixes[index]));
                }
                alternatives += ')';
            }
            alternatives += ')';
        }
        if (alternatives.empty()) {
            out.sql += "0";
            return {};
        }
        out.sql += std::format("EXISTS(SELECT 1 FROM locations fl WHERE fl.id={} AND ({}))",
                               detail::kPrimaryLocationIdSql, alternatives);
        return {};
    }

    /// collection:Name compares folded names (Unicode case-insensitive); the oldest live
    /// collection of that name wins.
    Result<void> compile_collection_name(const std::string& value, SqlPredicate& out) {
        auto lookup = connection_.prepare("SELECT id, name FROM collections WHERE deleted=0 ORDER BY id");
        if (!lookup) return std::unexpected(lookup.error());
        const std::string wanted = fold_key(value);
        std::optional<std::int64_t> found;
        while (!found) {
            auto has_row = lookup->next_row();
            if (!has_row) return std::unexpected(has_row.error());
            if (!*has_row) break;
            if (fold_key(lookup->column_text(1)) == wanted) found = lookup->column_int64(0);
        }
        lookup->reset();
        if (!found) {
            out.sql += "0";
            return {};
        }
        return compile_collection_id(*found, out);
    }

    sqlite::Connection& connection_;
    int nesting_;
    std::size_t& compiled_nodes_;
    std::optional<std::vector<RootFacts>> roots_;
};

}  // namespace

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
    std::size_t compiled_nodes = 0;
    return QueryCompiler(connection, 0, compiled_nodes).compile(query);
}

Result<SqlPredicate> compile_collection(std::int64_t collection_id, sqlite::Connection& connection) {
    std::size_t compiled_nodes = 0;
    SqlPredicate predicate;
    predicate.sql.clear();
    if (auto compiled = QueryCompiler(connection, 0, compiled_nodes).compile_collection_id(collection_id, predicate);
        !compiled)
        return std::unexpected(compiled.error());
    return predicate;
}

std::string_view availability_sql() {
    return kAvailableSql;
}

}  // namespace mso::search
