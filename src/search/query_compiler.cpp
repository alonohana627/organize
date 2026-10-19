#include "mso/search/query_compiler.hpp"

#include <format>

#include <nlohmann/json.hpp>

#include "mso/catalog/taxonomy_lookup.hpp"

namespace mso::search {

namespace {

constexpr int kMaxSmartCollectionNesting = 4;

constexpr std::string_view kAvailableSql =
    "EXISTS(SELECT 1 FROM locations al JOIN roots ar ON ar.id=al.root_id WHERE al.sample_id=s.id "
    "AND al.state='present' AND ar.availability<>'offline' AND ar.deleted=0)";

/// Escapes LIKE wildcards so user text matches literally (ESCAPE '\').
std::string escape_like(std::string_view text) {
    std::string escaped;
    for (const char character : text) {
        if (character == '%' || character == '_' || character == '\\') escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

class QueryCompiler {
public:
    QueryCompiler(sqlite::Connection& connection, int nesting) : connection_(connection), nesting_(nesting) {}

    Result<SqlPredicate> compile(const Query& query) {
        SqlPredicate predicate;
        if (query.empty()) return predicate;
        predicate.sql.clear();
        if (auto compiled = compile_node(*query.root, predicate); !compiled) return std::unexpected(compiled.error());
        return predicate;
    }

private:
    Result<void> compile_node(const QueryNode& node, SqlPredicate& out) {
        switch (node.kind) {
            case QueryNode::Kind::And:
            case QueryNode::Kind::Or: {
                const std::string_view joiner = node.kind == QueryNode::Kind::And ? " AND " : " OR ";
                out.sql += '(';
                for (std::size_t index = 0; index < node.children.size(); ++index) {
                    if (index) out.sql += joiner;
                    if (auto compiled = compile_node(*node.children[index], out); !compiled) return compiled;
                }
                out.sql += ')';
                return {};
            }
            case QueryNode::Kind::Not:
                out.sql += "NOT (";
                if (auto compiled = compile_node(*node.children.front(), out); !compiled) return compiled;
                out.sql += ')';
                return {};
            case QueryNode::Kind::Text:
                out.sql += "s.id IN (SELECT rowid FROM sample_fts WHERE sample_fts MATCH ?)";
                out.parameters.emplace_back(fts_expression(node.text));
                return {};
            case QueryNode::Kind::Predicate:
                return compile_predicate(node.predicate, out);
        }
        return fail(ErrorCode::Internal, "unknown query node");
    }

    static void compile_numeric(std::string_view column, const FieldPredicate& predicate, SqlPredicate& out) {
        switch (predicate.comparison) {
            case Comparison::Equal:
                out.sql += std::format("{} = ?", column);
                break;
            case Comparison::Less:
                out.sql += std::format("{} < ?", column);
                break;
            case Comparison::LessOrEqual:
                out.sql += std::format("{} <= ?", column);
                break;
            case Comparison::Greater:
                out.sql += std::format("{} > ?", column);
                break;
            case Comparison::GreaterOrEqual:
                out.sql += std::format("{} >= ?", column);
                break;
            case Comparison::Between:
                out.sql += std::format("{} BETWEEN ? AND ?", column);
                out.parameters.emplace_back(predicate.low);
                out.parameters.emplace_back(predicate.high);
                return;
        }
        out.parameters.emplace_back(predicate.low);
    }

    Result<void> compile_predicate(const FieldPredicate& predicate, SqlPredicate& out) {
        switch (predicate.field) {
            case Field::Tag:
                return compile_taxonomy(predicate.text, catalog::TaxonomyKind::Tags, out);
            case Field::Category:
                return compile_taxonomy(predicate.text, catalog::TaxonomyKind::Categories, out);
            case Field::Folder:
                return compile_folder(predicate.text, out);
            case Field::Collection:
                return compile_collection(predicate.text, out);
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
                out.sql += "(COALESCE(e.analysis_flags,0) & ?) <> 0";
                out.parameters.emplace_back(predicate.low);
                return {};
            case Field::Format:
                out.sql += "e.format = ?";
                out.parameters.emplace_back(predicate.text);
                return {};
            case Field::Codec:
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

    /// tag:/category: match the whole subtree. A single-segment value matches every node with
    /// that name at any depth ("tag:Kick" finds "Drums/Kick").
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
            seed = "SELECT ?";
            out.parameters.emplace_back(**node);
        }
        const std::string subtree = std::format(
            "WITH RECURSIVE subtree(id,depth) AS (SELECT id,0 FROM ({0}) UNION ALL "
            "SELECT t.id,subtree.depth+1 FROM {1} t JOIN subtree ON t.parent_id=subtree.id WHERE subtree.depth<16) "
            "SELECT id FROM subtree",
            seed, table);
        // The CTE parameter was pushed before this text; it is the first '?' in this fragment.
        out.sql +=
            tags ? std::format("EXISTS(SELECT 1 FROM sample_tags st WHERE st.sample_id=s.id AND st.tag_id IN ({}))",
                               subtree)
                 : std::format("s.category_id IN ({})", subtree);
        return {};
    }

    /// folder:"Packs/Goa" matches files under that relative folder in any root, or under
    /// "<root name>/<folder>".
    Result<void> compile_folder(const std::string& value, SqlPredicate& out) {
        std::string key = fold_path_key(value);
        while (key.ends_with('/'))
            key.pop_back();
        const std::string pattern = escape_like(key) + "/%";
        out.sql +=
            "EXISTS(SELECT 1 FROM locations fl JOIN roots fr ON fr.id=fl.root_id WHERE fl.sample_id=s.id "
            "AND fl.state<>'retired' AND (fl.path_key LIKE ? ESCAPE '\\' OR lower(fr.name) || '/' || fl.path_key "
            "LIKE ? ESCAPE '\\' OR lower(fr.name) = ?))";
        out.parameters.emplace_back(pattern);
        out.parameters.emplace_back(pattern);
        out.parameters.emplace_back(key);
        return {};
    }

    /// collection:Name — manual membership, or the stored query of a smart collection.
    Result<void> compile_collection(const std::string& value, SqlPredicate& out) {
        auto lookup = connection_.prepare(
            "SELECT id, kind, query_json FROM collections WHERE deleted=0 AND lower(name)=lower(?1) LIMIT 1");
        if (!lookup) return std::unexpected(lookup.error());
        lookup->bind(1, value);
        auto has_row = lookup->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) {
            out.sql += "0";
            return {};
        }
        const std::int64_t id = lookup->column_int64(0);
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
        const auto document = nlohmann::json::parse(stored_query, nullptr, false);
        const std::string text = document.is_object() ? document.value("text", std::string()) : std::string();
        auto parsed = parse_query(text);
        if (!parsed)
            return fail(ErrorCode::InvalidArgument, std::format("smart collection '{}' has an invalid query", value));
        auto compiled = QueryCompiler(connection_, nesting_ + 1).compile(*parsed);
        if (!compiled) return std::unexpected(compiled.error());
        out.sql += '(' + compiled->sql + ')';
        out.parameters.insert(out.parameters.end(), compiled->parameters.begin(), compiled->parameters.end());
        return {};
    }

    sqlite::Connection& connection_;
    int nesting_;
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
    return QueryCompiler(connection, 0).compile(query);
}

std::string_view availability_sql() {
    return kAvailableSql;
}

}  // namespace mso::search
