#include "mso/catalog/detail/patch_operations.hpp"

#include <format>

#include "mso/domain/core.hpp"

namespace mso::catalog::detail {

namespace patch {
nlohmann::json field(std::int64_t sample_id, std::string_view field, nlohmann::json before, nlohmann::json after) {
    return nlohmann::json{{"op", "field"},
                          {"sample", sample_id},
                          {"field", field},
                          {"before", std::move(before)},
                          {"after", std::move(after)}};
}
nlohmann::json tag_link(std::int64_t sample_id, std::int64_t tag_id, bool before, bool after) {
    return nlohmann::json{
        {"op", "tag_link"}, {"sample", sample_id}, {"tag", tag_id}, {"before", before}, {"after", after}};
}
nlohmann::json tag_links(std::int64_t tag_id, std::span<const std::int64_t> sample_ids, bool before, bool after) {
    return nlohmann::json{{"op", "tag_links"},
                          {"tag", tag_id},
                          {"samples", std::vector<std::int64_t>(sample_ids.begin(), sample_ids.end())},
                          {"before", before},
                          {"after", after}};
}
nlohmann::json collection_link(std::int64_t collection_id, std::int64_t sample_id, nlohmann::json before,
                               nlohmann::json after) {
    return nlohmann::json{{"op", "collection_link"},
                          {"collection", collection_id},
                          {"sample", sample_id},
                          {"before", std::move(before)},
                          {"after", std::move(after)}};
}
nlohmann::json tag_row(std::int64_t tag_id, std::string_view field, nlohmann::json before, nlohmann::json after) {
    return nlohmann::json{{"op", "tag_row"},
                          {"tag", tag_id},
                          {"field", field},
                          {"before", std::move(before)},
                          {"after", std::move(after)}};
}
nlohmann::json category_row(std::int64_t category_id, std::string_view field, nlohmann::json before,
                            nlohmann::json after) {
    return nlohmann::json{{"op", "category_row"},
                          {"category", category_id},
                          {"field", field},
                          {"before", std::move(before)},
                          {"after", std::move(after)}};
}
nlohmann::json collection_row(std::int64_t collection_id, std::string_view field, nlohmann::json before,
                              nlohmann::json after) {
    return nlohmann::json{{"op", "collection_row"},
                          {"collection", collection_id},
                          {"field", field},
                          {"before", std::move(before)},
                          {"after", std::move(after)}};
}
}  // namespace patch

namespace {

constexpr std::int64_t kMinimumMilliBpm = 20'000;
constexpr std::int64_t kMaximumMilliBpm = 400'000;

/// Kinds of stored operations.
enum class OperationKind { Field, TagLink, TagLinks, CollectionLink, TagRow, CategoryRow, CollectionRow };

/// A stored operation after validation of its shape (values are validated when written).
struct Operation {
    OperationKind kind = OperationKind::Field;
    std::int64_t sample_id = 0;            ///< field, tag_link, collection_link
    std::int64_t target_id = 0;            ///< tag, collection or category id
    std::string field;                     ///< field and *_row operations
    std::vector<std::int64_t> sample_ids;  ///< tag_links
    const nlohmann::json* before = nullptr;
    const nlohmann::json* after = nullptr;
};

std::unexpected<Error> corrupt_operation(const nlohmann::json& operation, std::string_view why) {
    return fail(ErrorCode::DbCorrupt,
                std::format("malformed history operation ({}): {}", why, to_json_text(operation).substr(0, 200)));
}

std::unexpected<Error> corrupt_value(std::string_view field, const nlohmann::json& value) {
    return fail(ErrorCode::DbCorrupt,
                std::format("history value for '{}' is invalid: {}", field, to_json_text(value).substr(0, 120)));
}

bool is_one_of(std::string_view value, std::initializer_list<std::string_view> allowed) {
    for (const auto candidate : allowed)
        if (candidate == value) return true;
    return false;
}

Result<Operation> parse_operation(const nlohmann::json& stored) {
    Operation operation;
    const auto kind = json_string_member(stored, "op");
    if (!kind) return corrupt_operation(stored, "no op");
    operation.before = json_member(stored, "before");
    operation.after = json_member(stored, "after");
    if (!operation.before || !operation.after) return corrupt_operation(stored, "no before/after");

    auto require_integer = [&](std::string_view key, std::int64_t& out) -> Result<void> {
        const auto value = json_integer_member(stored, key);
        if (!value) return corrupt_operation(stored, std::format("no integer '{}'", key));
        out = *value;
        return {};
    };
    auto require_field = [&](std::initializer_list<std::string_view> allowed) -> Result<void> {
        auto field = json_string_member(stored, "field");
        if (!field || !is_one_of(*field, allowed)) return corrupt_operation(stored, "unknown field");
        operation.field = std::move(*field);
        return {};
    };

    Result<void> parsed;
    if (*kind == "field") {
        operation.kind = OperationKind::Field;
        parsed = require_integer("sample", operation.sample_id);
        if (parsed)
            parsed = require_field({"display_name", "rating", "favorite", "notes", "category_id", "bpm_override",
                                    "key_override", "deleted"});
    } else if (*kind == "tag_link") {
        operation.kind = OperationKind::TagLink;
        parsed = require_integer("sample", operation.sample_id);
        if (parsed) parsed = require_integer("tag", operation.target_id);
    } else if (*kind == "tag_links") {
        operation.kind = OperationKind::TagLinks;
        parsed = require_integer("tag", operation.target_id);
        auto samples = json_integer_array_member(stored, "samples");
        if (parsed && !samples) return corrupt_operation(stored, "no sample list");
        if (samples) operation.sample_ids = std::move(*samples);
    } else if (*kind == "collection_link") {
        operation.kind = OperationKind::CollectionLink;
        parsed = require_integer("collection", operation.target_id);
        if (parsed) parsed = require_integer("sample", operation.sample_id);
    } else if (*kind == "tag_row") {
        operation.kind = OperationKind::TagRow;
        parsed = require_integer("tag", operation.target_id);
        if (parsed) parsed = require_field({"name", "parent_id", "alias_of", "deleted"});
    } else if (*kind == "category_row") {
        operation.kind = OperationKind::CategoryRow;
        parsed = require_integer("category", operation.target_id);
        if (parsed) parsed = require_field({"name", "parent_id", "deleted"});
    } else if (*kind == "collection_row") {
        operation.kind = OperationKind::CollectionRow;
        parsed = require_integer("collection", operation.target_id);
        if (parsed) parsed = require_field({"name", "deleted"});
    } else {
        return corrupt_operation(stored, "unknown op");
    }
    if (!parsed) return std::unexpected(parsed.error());
    return operation;
}

nlohmann::json optional_int_json(const std::optional<std::int64_t>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json();
}

/// Runs a single-row SELECT and converts the first row with `convert`; NOT_FOUND when absent.
Result<nlohmann::json> select_one(sqlite::Connection& connection, std::string_view sql, std::int64_t id,
                                  const std::function<nlohmann::json(sqlite::PreparedStatement&)>& convert) {
    auto statement = connection.prepare(sql);
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, id);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::NotFound, std::format("row {} not found", id));
    nlohmann::json value = convert(*statement);
    statement->reset();
    return value;
}

// ---------------------------------------------------------------- sample fields

Result<nlohmann::json> read_sample_field(sqlite::Connection& connection, std::int64_t sample_id,
                                         const std::string& field) {
    if (field == "display_name")
        return select_one(connection, "SELECT display_name FROM samples WHERE id=?1", sample_id,
                          [](auto& row) { return nlohmann::json(row.column_text(0)); });
    if (field == "notes")
        return select_one(connection, "SELECT notes FROM samples WHERE id=?1", sample_id,
                          [](auto& row) { return nlohmann::json(row.column_text(0)); });
    if (field == "rating")
        return select_one(connection, "SELECT rating FROM samples WHERE id=?1", sample_id,
                          [](auto& row) { return nlohmann::json(row.column_int64(0)); });
    if (field == "favorite")
        return select_one(connection, "SELECT favorite FROM samples WHERE id=?1", sample_id,
                          [](auto& row) { return nlohmann::json(row.column_int64(0) != 0); });
    if (field == "deleted")
        return select_one(connection, "SELECT deleted FROM samples WHERE id=?1", sample_id,
                          [](auto& row) { return nlohmann::json(row.column_int64(0) != 0); });
    if (field == "category_id")
        return select_one(connection, "SELECT category_id FROM samples WHERE id=?1", sample_id,
                          [](auto& row) { return optional_int_json(row.column_optional_int64(0)); });
    if (field == "bpm_override")
        return select_one(connection, "SELECT bpm_override_state, bpm_override_milli FROM samples WHERE id=?1",
                          sample_id, [](auto& row) {
                              return nlohmann::json{{"state", row.column_text(0)},
                                                    {"milli", optional_int_json(row.column_optional_int64(1))}};
                          });
    if (field == "key_override")
        return select_one(connection,
                          "SELECT key_override_state, key_override_class, key_override_mode FROM samples WHERE id=?1",
                          sample_id, [](auto& row) {
                              auto mode = row.column_optional_text(2);
                              return nlohmann::json{{"state", row.column_text(0)},
                                                    {"pitch_class", optional_int_json(row.column_optional_int64(1))},
                                                    {"mode", mode ? nlohmann::json(*mode) : nlohmann::json()}};
                          });
    return fail(ErrorCode::DbCorrupt, std::format("unknown sample field '{}'", field));
}

/// Binds the new value of a sample field after checking its type and range (stored history may be
/// corrupt). Returns the statement ready to run with ?1 = sample id.
Result<sqlite::PreparedStatement> prepare_sample_field_write(sqlite::Connection& connection, const std::string& field,
                                                             const nlohmann::json& value) {
    auto prepared = [&](std::string_view sql) { return connection.prepare(sql); };
    if (field == "display_name" || field == "notes") {
        if (!value.is_string()) return corrupt_value(field, value);
        const auto& text = value.get_ref<const nlohmann::json::string_t&>();
        if (!is_valid_utf8(text) || (field == "display_name" && trim(text).empty())) return corrupt_value(field, value);
        auto statement = field == "display_name"
                             ? prepared("UPDATE samples SET display_name=?2, name_key=?3 WHERE id=?1")
                             : prepared("UPDATE samples SET notes=?2 WHERE id=?1");
        if (!statement) return statement;
        statement->bind(2, text);
        if (field == "display_name") statement->bind(3, fold_key(text));
        return statement;
    }
    if (field == "rating") {
        const auto rating = json_integer(value);
        if (!rating || *rating < 0 || *rating > 5) return corrupt_value(field, value);
        auto statement = prepared("UPDATE samples SET rating=?2 WHERE id=?1");
        if (statement) statement->bind(2, *rating);
        return statement;
    }
    if (field == "favorite" || field == "deleted") {
        if (!value.is_boolean()) return corrupt_value(field, value);
        auto statement = field == "favorite" ? prepared("UPDATE samples SET favorite=?2 WHERE id=?1")
                                             : prepared("UPDATE samples SET deleted=?2 WHERE id=?1");
        if (statement) statement->bind(2, value.get<bool>() ? 1 : 0);
        return statement;
    }
    if (field == "category_id") {
        const auto category = json_integer(value);
        if (!value.is_null() && !category) return corrupt_value(field, value);
        auto statement = prepared("UPDATE samples SET category_id=?2 WHERE id=?1");
        if (statement) statement->bind(2, category);
        return statement;
    }
    if (field == "bpm_override") {
        const auto state = json_string_member(value, "state");
        const auto* milli_value = json_member(value, "milli");
        const auto milli = milli_value ? json_integer(*milli_value) : std::nullopt;
        const bool valid_state = state && is_one_of(*state, {"auto", "value", "unknown"});
        const bool needs_value = state && *state == "value";
        if (!valid_state || !milli_value || needs_value != milli.has_value() ||
            (milli && (*milli < kMinimumMilliBpm || *milli > kMaximumMilliBpm)) ||
            (!needs_value && !milli_value->is_null()))
            return corrupt_value(field, value);
        auto statement = prepared("UPDATE samples SET bpm_override_state=?2, bpm_override_milli=?3 WHERE id=?1");
        if (statement) statement->bind(2, *state).bind(3, milli);
        return statement;
    }
    if (field == "key_override") {
        const auto state = json_string_member(value, "state");
        const auto pitch_class = json_integer_member(value, "pitch_class");
        const auto mode = json_string_member(value, "mode");
        const bool valid_state = state && is_one_of(*state, {"auto", "value", "unknown"});
        const bool needs_value = state && *state == "value";
        const bool has_value = pitch_class && mode;
        if (!valid_state || needs_value != has_value || (pitch_class && (*pitch_class < 0 || *pitch_class > 11)) ||
            (mode && !parse_key_mode(*mode)))
            return corrupt_value(field, value);
        auto statement = prepared(
            "UPDATE samples SET key_override_state=?2, key_override_class=?3, key_override_mode=?4 WHERE id=?1");
        if (statement) statement->bind(2, *state).bind(3, needs_value ? pitch_class : std::nullopt);
        if (statement) statement->bind(4, needs_value ? mode : std::nullopt);
        return statement;
    }
    return fail(ErrorCode::DbCorrupt, std::format("unknown sample field '{}'", field));
}

Result<void> write_sample_field(sqlite::Connection& connection, std::int64_t sample_id, const std::string& field,
                                const nlohmann::json& value, PatchEffects& effects) {
    auto statement = prepare_sample_field_write(connection, field, value);
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, sample_id);
    if (auto written = statement->run(); !written) return written;

    auto touch = connection.prepare("UPDATE samples SET edit_revision=edit_revision+1, updated_at=?2 WHERE id=?1");
    if (!touch) return std::unexpected(touch.error());
    touch->bind(1, sample_id).bind(2, now_unix_ms());
    if (auto touched = touch->run(); !touched) return touched;

    if (field == "bpm_override" || field == "key_override")
        if (auto recomputed = recompute_effective_musical_facts(connection, sample_id); !recomputed) return recomputed;
    if (field == "category_id" || field == "deleted") effects.structure_changed = true;  // navigator counts
    const bool search_text =
        field == "display_name" || field == "notes" || field == "category_id" || field == "deleted";
    return mark_sample_changed(connection, sample_id, search_text);
}

// ---------------------------------------------------------------- memberships

Result<nlohmann::json> read_tag_link(sqlite::Connection& connection, const Operation& operation) {
    auto statement = connection.prepare("SELECT 1 FROM sample_tags WHERE sample_id=?1 AND tag_id=?2");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, operation.sample_id).bind(2, operation.target_id);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    statement->reset();
    return nlohmann::json(*has_row);
}

Result<void> write_tag_link(sqlite::Connection& connection, const Operation& operation, bool linked,
                            PatchEffects& effects) {
    auto statement = connection.prepare(linked ? "INSERT INTO sample_tags(sample_id,tag_id,created_at) VALUES(?1,?2,?3)"
                                               : "DELETE FROM sample_tags WHERE sample_id=?1 AND tag_id=?2");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, operation.sample_id).bind(2, operation.target_id);
    if (linked) statement->bind(3, now_unix_ms());
    if (auto written = statement->run(); !written) return written;
    effects.structure_changed = true;  // tag sample counts
    return mark_sample_changed(connection, operation.sample_id, /*search_text_changed=*/true);
}

std::string sample_list_json(const Operation& operation) {
    return to_json_text(nlohmann::json(operation.sample_ids));
}

/// true when every listed sample is linked, false when none is, null when they are mixed.
Result<nlohmann::json> read_tag_links(sqlite::Connection& connection, const Operation& operation) {
    if (operation.sample_ids.empty()) return nlohmann::json(true);
    auto statement = connection.prepare(
        "SELECT (SELECT count(DISTINCT value) FROM json_each(?1)), (SELECT count(*) FROM sample_tags link "
        "WHERE link.tag_id=?2 AND link.sample_id IN (SELECT value FROM json_each(?1)))");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, sample_list_json(operation)).bind(2, operation.target_id);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::Internal, "aggregate query returned no row");
    const auto listed = statement->column_int64(0);
    const auto linked = statement->column_int64(1);
    statement->reset();
    if (linked == listed) return nlohmann::json(true);
    if (linked == 0) return nlohmann::json(false);
    return nlohmann::json();
}

Result<void> write_tag_links(sqlite::Connection& connection, const Operation& operation, bool linked,
                             PatchEffects& effects) {
    if (operation.sample_ids.empty()) return {};
    auto statement = connection.prepare(
        linked ? "INSERT INTO sample_tags(sample_id,tag_id,created_at) SELECT DISTINCT value, ?2, ?3 FROM json_each(?1)"
               : "DELETE FROM sample_tags WHERE tag_id=?2 AND sample_id IN (SELECT value FROM json_each(?1))");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, sample_list_json(operation)).bind(2, operation.target_id);
    if (linked) statement->bind(3, now_unix_ms());
    if (auto written = statement->run(); !written) return written;
    effects.structure_changed = true;
    return mark_sample_list_changed(connection, operation.sample_ids, /*search_text_changed=*/true);
}

Result<nlohmann::json> read_collection_link(sqlite::Connection& connection, const Operation& operation) {
    auto statement = connection.prepare("SELECT ordinal FROM collection_items WHERE collection_id=?1 AND sample_id=?2");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, operation.target_id).bind(2, operation.sample_id);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    nlohmann::json value = *has_row ? nlohmann::json(statement->column_int64(0)) : nlohmann::json();
    statement->reset();
    return value;
}

Result<void> write_collection_link(sqlite::Connection& connection, const Operation& operation,
                                   const nlohmann::json& ordinal, PatchEffects& effects) {
    effects.structure_changed = true;  // collection counts
    if (ordinal.is_null()) {
        auto remove = connection.prepare("DELETE FROM collection_items WHERE collection_id=?1 AND sample_id=?2");
        if (!remove) return std::unexpected(remove.error());
        remove->bind(1, operation.target_id).bind(2, operation.sample_id);
        if (auto removed = remove->run(); !removed) return removed;
        return mark_sample_changed(connection, operation.sample_id, /*search_text_changed=*/false);
    }
    const auto position = json_integer(ordinal);
    if (!position) return corrupt_value("ordinal", ordinal);
    auto insert = connection.prepare("INSERT INTO collection_items(collection_id,sample_id,ordinal) VALUES(?1,?2,?3)");
    if (!insert) return std::unexpected(insert.error());
    insert->bind(1, operation.target_id).bind(2, operation.sample_id).bind(3, *position);
    if (auto inserted = insert->run(); !inserted) {
        // The original position was taken by a later edit: a conflict, not a silent reorder. Every
        // other failure (busy, disk full, I/O) is reported as what it is.
        if (insert->failed_on_unique_constraint())
            return fail(ErrorCode::HistoryConflict, "collection position is now occupied by another sample");
        return inserted;
    }
    return mark_sample_changed(connection, operation.sample_id, /*search_text_changed=*/false);
}

// ---------------------------------------------------------------- taxonomy and collection rows

std::string_view row_table(OperationKind kind) {
    switch (kind) {
        case OperationKind::TagRow:
            return "tags";
        case OperationKind::CategoryRow:
            return "categories";
        default:
            return "collections";
    }
}

Result<nlohmann::json> read_row_field(sqlite::Connection& connection, std::string_view table, std::int64_t id,
                                      const std::string& field) {
    if (field == "alias_of")
        return select_one(connection,
                          "SELECT (SELECT canonical_tag_id FROM tag_aliases WHERE source_tag_id=tag.id) "
                          "FROM tags tag WHERE tag.id=?1",
                          id, [](auto& row) { return optional_int_json(row.column_optional_int64(0)); });
    if (field == "name")
        return select_one(connection, std::format("SELECT name FROM {} WHERE id=?1", table), id,
                          [](auto& row) { return nlohmann::json(row.column_text(0)); });
    if (field == "parent_id")
        return select_one(connection, std::format("SELECT parent_id FROM {} WHERE id=?1", table), id,
                          [](auto& row) { return optional_int_json(row.column_optional_int64(0)); });
    if (field == "deleted")
        return select_one(connection, std::format("SELECT deleted FROM {} WHERE id=?1", table), id,
                          [](auto& row) { return nlohmann::json(row.column_int64(0) != 0); });
    return fail(ErrorCode::DbCorrupt, std::format("unknown {} field '{}'", table, field));
}

Result<void> write_row_field(sqlite::Connection& connection, std::string_view table, std::int64_t id,
                             const std::string& field, const nlohmann::json& value) {
    if (field == "alias_of") {
        const auto canonical = json_integer(value);
        if (!value.is_null() && !canonical) return corrupt_value(field, value);
        auto remove = connection.prepare("DELETE FROM tag_aliases WHERE source_tag_id=?1");
        if (!remove) return std::unexpected(remove.error());
        remove->bind(1, id);
        if (auto removed = remove->run(); !removed) return removed;
        if (!canonical) return {};
        auto insert = connection.prepare("INSERT INTO tag_aliases(source_tag_id,canonical_tag_id) VALUES(?1,?2)");
        if (!insert) return std::unexpected(insert.error());
        insert->bind(1, id).bind(2, *canonical);
        return insert->run();
    }
    Result<sqlite::PreparedStatement> statement = fail(ErrorCode::Internal, "unprepared");
    if (field == "name") {
        if (!value.is_string() || !is_valid_utf8(value.get_ref<const nlohmann::json::string_t&>()))
            return corrupt_value(field, value);
        const auto& name = value.get_ref<const nlohmann::json::string_t&>();
        const bool has_key = table != "collections";
        statement = connection.prepare(
            has_key ? std::format("UPDATE {} SET name=?2, name_key=?3, revision=revision+1 WHERE id=?1", table)
                    : std::format("UPDATE {} SET name=?2, revision=revision+1 WHERE id=?1", table));
        if (!statement) return std::unexpected(statement.error());
        statement->bind(2, name);
        if (has_key) statement->bind(3, fold_key(name));
    } else if (field == "parent_id") {
        const auto parent = json_integer(value);
        if (!value.is_null() && !parent) return corrupt_value(field, value);
        statement =
            connection.prepare(std::format("UPDATE {} SET parent_id=?2, revision=revision+1 WHERE id=?1", table));
        if (!statement) return std::unexpected(statement.error());
        statement->bind(2, parent);
    } else if (field == "deleted") {
        if (!value.is_boolean()) return corrupt_value(field, value);
        statement = connection.prepare(std::format("UPDATE {} SET deleted=?2, revision=revision+1 WHERE id=?1", table));
        if (!statement) return std::unexpected(statement.error());
        statement->bind(2, value.get<bool>() ? 1 : 0);
    } else {
        return fail(ErrorCode::DbCorrupt, std::format("unknown {} field '{}'", table, field));
    }
    statement->bind(1, id);
    if (auto written = statement->run(); !written) {
        if (statement->failed_on_unique_constraint())  // the sibling name is now taken by another entry
            return fail(ErrorCode::HistoryConflict, std::format("another {} entry now uses this name", table));
        return written;
    }
    return {};
}

/// SQL selecting (as `sample_id`) the samples whose tag or category path runs through node ?1.
std::string_view subtree_members_sql(Taxonomy taxonomy) {
    return taxonomy == Taxonomy::Tags
               ? "WITH RECURSIVE subtree(id,depth) AS (SELECT ?1,0 UNION ALL SELECT tag.id,subtree.depth+1 FROM tags "
                 "tag JOIN subtree ON tag.parent_id=subtree.id WHERE subtree.depth<16) "
                 "SELECT link.sample_id AS sample_id FROM sample_tags link JOIN subtree ON link.tag_id=subtree.id"
               : "WITH RECURSIVE subtree(id,depth) AS (SELECT ?1,0 UNION ALL SELECT category.id,subtree.depth+1 FROM "
                 "categories category JOIN subtree ON category.parent_id=subtree.id WHERE subtree.depth<16) "
                 "SELECT sample.id AS sample_id FROM samples sample JOIN subtree ON sample.category_id=subtree.id";
}

Result<nlohmann::json> read_current(sqlite::Connection& connection, const Operation& operation) {
    switch (operation.kind) {
        case OperationKind::Field:
            return read_sample_field(connection, operation.sample_id, operation.field);
        case OperationKind::TagLink:
            return read_tag_link(connection, operation);
        case OperationKind::TagLinks:
            return read_tag_links(connection, operation);
        case OperationKind::CollectionLink:
            return read_collection_link(connection, operation);
        case OperationKind::TagRow:
        case OperationKind::CategoryRow:
        case OperationKind::CollectionRow:
            return read_row_field(connection, row_table(operation.kind), operation.target_id, operation.field);
    }
    return fail(ErrorCode::DbCorrupt, "unknown patch operation");
}

}  // namespace

Result<nlohmann::json> read_current_value(sqlite::Connection& connection, const nlohmann::json& operation) {
    auto parsed = parse_operation(operation);
    if (!parsed) return std::unexpected(parsed.error());
    return read_current(connection, *parsed);
}

Result<void> apply_operation(sqlite::Connection& connection, const nlohmann::json& stored, PatchDirection direction,
                             PatchEffects& effects) {
    auto parsed = parse_operation(stored);
    if (!parsed) return std::unexpected(parsed.error());
    const Operation& operation = *parsed;
    const nlohmann::json& expected = direction == PatchDirection::Forward ? *operation.before : *operation.after;
    const nlohmann::json& target = direction == PatchDirection::Forward ? *operation.after : *operation.before;
    auto current = read_current(connection, operation);
    if (!current) return std::unexpected(current.error());
    if (*current != expected)
        return fail(ErrorCode::HistoryConflict,
                    std::format("{} changed since: expected {}, found {}", to_json_text(stored).substr(0, 200),
                                to_json_text(expected), to_json_text(*current)));
    if (*current == target) return {};

    switch (operation.kind) {
        case OperationKind::Field:
            return write_sample_field(connection, operation.sample_id, operation.field, target, effects);
        case OperationKind::TagLink:
        case OperationKind::TagLinks: {
            if (!target.is_boolean()) return corrupt_value("tag link", target);
            return operation.kind == OperationKind::TagLink
                       ? write_tag_link(connection, operation, target.get<bool>(), effects)
                       : write_tag_links(connection, operation, target.get<bool>(), effects);
        }
        case OperationKind::CollectionLink:
            return write_collection_link(connection, operation, target, effects);
        case OperationKind::TagRow:
        case OperationKind::CategoryRow: {
            const bool is_tag = operation.kind == OperationKind::TagRow;
            if (auto written = write_row_field(connection, row_table(operation.kind), operation.target_id,
                                               operation.field, target);
                !written)
                return written;
            effects.structure_changed = true;
            if (operation.field == "name" || operation.field == "parent_id") {
                const std::int64_t node = operation.target_id;
                return mark_samples_changed(connection,
                                            subtree_members_sql(is_tag ? Taxonomy::Tags : Taxonomy::Categories),
                                            std::span<const std::int64_t>(&node, 1), /*search_text_changed=*/true);
            }
            return {};
        }
        case OperationKind::CollectionRow:
            effects.structure_changed = true;
            return write_row_field(connection, "collections", operation.target_id, operation.field, target);
    }
    return fail(ErrorCode::DbCorrupt, "unknown patch operation");
}

}  // namespace mso::catalog::detail
