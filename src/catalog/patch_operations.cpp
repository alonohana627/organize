#include "mso/catalog/patch_operations.hpp"

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

nlohmann::json optional_int_json(const std::optional<std::int64_t>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json();
}

std::optional<std::int64_t> json_optional_int(const nlohmann::json& value) {
    return value.is_number_integer() ? std::optional<std::int64_t>(value.get<std::int64_t>()) : std::nullopt;
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
    if (field == "display_name" || field == "notes")
        return select_one(connection, std::format("SELECT {} FROM samples WHERE id=?1", field), sample_id,
                          [](auto& row) { return nlohmann::json(row.column_text(0)); });
    if (field == "rating")
        return select_one(connection, "SELECT rating FROM samples WHERE id=?1", sample_id,
                          [](auto& row) { return nlohmann::json(row.column_int64(0)); });
    if (field == "favorite" || field == "deleted")
        return select_one(connection, std::format("SELECT {} FROM samples WHERE id=?1", field), sample_id,
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
    return fail(ErrorCode::InvalidArgument, std::format("unknown sample field '{}'", field));
}

Result<void> write_sample_field(sqlite::Connection& connection, std::int64_t sample_id, const std::string& field,
                                const nlohmann::json& value, std::set<std::int64_t>& affected) {
    std::string sql;
    auto statement = Result<sqlite::PreparedStatement>(fail(ErrorCode::Internal, "unprepared"));
    if (field == "display_name") {
        statement = connection.prepare("UPDATE samples SET display_name=?2, name_key=?3 WHERE id=?1");
        if (!statement) return std::unexpected(statement.error());
        const auto name = value.get<std::string>();
        statement->bind(2, name).bind(3, fold_key(name));
    } else if (field == "notes") {
        statement = connection.prepare("UPDATE samples SET notes=?2 WHERE id=?1");
        if (!statement) return std::unexpected(statement.error());
        statement->bind(2, value.get<std::string>());
    } else if (field == "rating") {
        statement = connection.prepare("UPDATE samples SET rating=?2 WHERE id=?1");
        if (!statement) return std::unexpected(statement.error());
        statement->bind(2, value.get<std::int64_t>());
    } else if (field == "favorite" || field == "deleted") {
        statement = connection.prepare(std::format("UPDATE samples SET {}=?2 WHERE id=?1", field));
        if (!statement) return std::unexpected(statement.error());
        statement->bind(2, value.get<bool>() ? 1 : 0);
    } else if (field == "category_id") {
        statement = connection.prepare("UPDATE samples SET category_id=?2 WHERE id=?1");
        if (!statement) return std::unexpected(statement.error());
        statement->bind(2, json_optional_int(value));
    } else if (field == "bpm_override") {
        statement = connection.prepare("UPDATE samples SET bpm_override_state=?2, bpm_override_milli=?3 WHERE id=?1");
        if (!statement) return std::unexpected(statement.error());
        statement->bind(2, value["state"].get<std::string>()).bind(3, json_optional_int(value["milli"]));
    } else if (field == "key_override") {
        statement = connection.prepare(
            "UPDATE samples SET key_override_state=?2, key_override_class=?3, key_override_mode=?4 WHERE id=?1");
        if (!statement) return std::unexpected(statement.error());
        statement->bind(2, value["state"].get<std::string>()).bind(3, json_optional_int(value["pitch_class"]));
        if (value["mode"].is_string())
            statement->bind(4, value["mode"].get<std::string>());
        else
            statement->bind_null(4);
    } else {
        return fail(ErrorCode::InvalidArgument, std::format("unknown sample field '{}'", field));
    }
    statement->bind(1, sample_id);
    if (auto written = statement->run(); !written) return written;

    auto touch = connection.prepare("UPDATE samples SET edit_revision=edit_revision+1, updated_at=?2 WHERE id=?1");
    if (!touch) return std::unexpected(touch.error());
    touch->bind(1, sample_id).bind(2, now_unix_ms());
    if (auto touched = touch->run(); !touched) return touched;

    if (field == "bpm_override" || field == "key_override")
        return recompute_effective_musical_facts(connection, sample_id);
    if (field == "display_name" || field == "notes" || field == "category_id" || field == "deleted")
        affected.insert(sample_id);
    return {};
}

// ---------------------------------------------------------------- memberships

Result<nlohmann::json> read_tag_link(sqlite::Connection& connection, const nlohmann::json& operation) {
    auto statement = connection.prepare("SELECT 1 FROM sample_tags WHERE sample_id=?1 AND tag_id=?2");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, operation["sample"].get<std::int64_t>()).bind(2, operation["tag"].get<std::int64_t>());
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    statement->reset();
    return nlohmann::json(*has_row);
}

Result<void> write_tag_link(sqlite::Connection& connection, const nlohmann::json& operation, bool linked,
                            std::set<std::int64_t>& affected) {
    const auto sample_id = operation["sample"].get<std::int64_t>();
    auto statement = connection.prepare(linked ? "INSERT INTO sample_tags(sample_id,tag_id,created_at) VALUES(?1,?2,?3)"
                                               : "DELETE FROM sample_tags WHERE sample_id=?1 AND tag_id=?2");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, sample_id).bind(2, operation["tag"].get<std::int64_t>());
    if (linked) statement->bind(3, now_unix_ms());
    if (auto written = statement->run(); !written) return written;
    affected.insert(sample_id);
    return {};
}

Result<nlohmann::json> read_collection_link(sqlite::Connection& connection, const nlohmann::json& operation) {
    auto statement = connection.prepare("SELECT ordinal FROM collection_items WHERE collection_id=?1 AND sample_id=?2");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, operation["collection"].get<std::int64_t>()).bind(2, operation["sample"].get<std::int64_t>());
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    nlohmann::json value = *has_row ? nlohmann::json(statement->column_int64(0)) : nlohmann::json();
    statement->reset();
    return value;
}

Result<void> write_collection_link(sqlite::Connection& connection, const nlohmann::json& operation,
                                   const nlohmann::json& ordinal) {
    const auto collection_id = operation["collection"].get<std::int64_t>();
    const auto sample_id = operation["sample"].get<std::int64_t>();
    if (ordinal.is_null()) {
        auto remove = connection.prepare("DELETE FROM collection_items WHERE collection_id=?1 AND sample_id=?2");
        if (!remove) return std::unexpected(remove.error());
        remove->bind(1, collection_id).bind(2, sample_id);
        return remove->run();
    }
    auto insert = connection.prepare("INSERT INTO collection_items(collection_id,sample_id,ordinal) VALUES(?1,?2,?3)");
    if (!insert) return std::unexpected(insert.error());
    insert->bind(1, collection_id).bind(2, sample_id).bind(3, ordinal.get<std::int64_t>());
    if (auto inserted = insert->run(); !inserted) {
        // The original position was taken by a later edit: a conflict, not a silent reorder.
        return fail(ErrorCode::HistoryConflict, "collection position is now occupied by another sample");
    }
    return {};
}

// ---------------------------------------------------------------- taxonomy and collection rows

Result<nlohmann::json> read_row_field(sqlite::Connection& connection, std::string_view table, std::int64_t id,
                                      const std::string& field) {
    if (field == "alias_of")
        return select_one(connection,
                          "SELECT (SELECT canonical_tag_id FROM tag_aliases WHERE source_tag_id=t.id) "
                          "FROM tags t WHERE t.id=?1",
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
    return fail(ErrorCode::InvalidArgument, std::format("unknown {} field '{}'", table, field));
}

Result<void> write_row_field(sqlite::Connection& connection, std::string_view table, std::int64_t id,
                             const std::string& field, const nlohmann::json& value) {
    if (field == "alias_of") {
        auto remove = connection.prepare("DELETE FROM tag_aliases WHERE source_tag_id=?1");
        if (!remove) return std::unexpected(remove.error());
        remove->bind(1, id);
        if (auto removed = remove->run(); !removed) return removed;
        if (value.is_null()) return {};
        auto insert = connection.prepare("INSERT INTO tag_aliases(source_tag_id,canonical_tag_id) VALUES(?1,?2)");
        if (!insert) return std::unexpected(insert.error());
        insert->bind(1, id).bind(2, value.get<std::int64_t>());
        return insert->run();
    }
    auto statement = Result<sqlite::PreparedStatement>(fail(ErrorCode::Internal, "unprepared"));
    if (field == "name") {
        const bool has_key = table != "collections";
        statement = connection.prepare(
            has_key ? std::format("UPDATE {} SET name=?2, name_key=?3, revision=revision+1 WHERE id=?1", table)
                    : std::format("UPDATE {} SET name=?2, revision=revision+1 WHERE id=?1", table));
        if (!statement) return std::unexpected(statement.error());
        const auto name = value.get<std::string>();
        statement->bind(2, name);
        if (has_key) statement->bind(3, fold_key(name));
    } else if (field == "parent_id") {
        statement =
            connection.prepare(std::format("UPDATE {} SET parent_id=?2, revision=revision+1 WHERE id=?1", table));
        if (!statement) return std::unexpected(statement.error());
        statement->bind(2, json_optional_int(value));
    } else if (field == "deleted") {
        statement = connection.prepare(std::format("UPDATE {} SET deleted=?2, revision=revision+1 WHERE id=?1", table));
        if (!statement) return std::unexpected(statement.error());
        statement->bind(2, value.get<bool>() ? 1 : 0);
    } else {
        return fail(ErrorCode::InvalidArgument, std::format("unknown {} field '{}'", table, field));
    }
    statement->bind(1, id);
    if (auto written = statement->run(); !written) {
        if (written.error().code == ErrorCode::InvalidArgument)  // unique sibling name now taken
            return fail(ErrorCode::HistoryConflict, std::format("another {} entry now uses this name", table));
        return written;
    }
    return {};
}

}  // namespace

Result<std::set<std::int64_t>> samples_in_subtree(sqlite::Connection& connection, Taxonomy taxonomy,
                                                  std::int64_t root_id) {
    const std::string sql =
        taxonomy == Taxonomy::Tags
            ? "WITH RECURSIVE subtree(id,depth) AS (SELECT ?1,0 UNION ALL SELECT t.id,s.depth+1 FROM tags t "
              "JOIN subtree s ON t.parent_id=s.id WHERE s.depth<16) "
              "SELECT DISTINCT st.sample_id FROM sample_tags st JOIN subtree s ON st.tag_id=s.id"
            : "WITH RECURSIVE subtree(id,depth) AS (SELECT ?1,0 UNION ALL SELECT c.id,s.depth+1 FROM categories c "
              "JOIN subtree s ON c.parent_id=s.id WHERE s.depth<16) "
              "SELECT DISTINCT sm.id FROM samples sm JOIN subtree s ON sm.category_id=s.id";
    auto statement = connection.prepare(sql);
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, root_id);
    std::set<std::int64_t> samples;
    auto walked = for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        samples.insert(row.column_int64(0));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return samples;
}

Result<nlohmann::json> read_current_value(sqlite::Connection& connection, const nlohmann::json& operation) {
    const auto kind = operation["op"].get<std::string>();
    if (kind == "field")
        return read_sample_field(connection, operation["sample"].get<std::int64_t>(),
                                 operation["field"].get<std::string>());
    if (kind == "tag_link") return read_tag_link(connection, operation);
    if (kind == "collection_link") return read_collection_link(connection, operation);
    if (kind == "tag_row")
        return read_row_field(connection, "tags", operation["tag"].get<std::int64_t>(),
                              operation["field"].get<std::string>());
    if (kind == "category_row")
        return read_row_field(connection, "categories", operation["category"].get<std::int64_t>(),
                              operation["field"].get<std::string>());
    if (kind == "collection_row")
        return read_row_field(connection, "collections", operation["collection"].get<std::int64_t>(),
                              operation["field"].get<std::string>());
    return fail(ErrorCode::InvalidArgument, std::format("unknown patch operation '{}'", kind));
}

Result<void> apply_operation(sqlite::Connection& connection, const nlohmann::json& operation, PatchDirection direction,
                             std::set<std::int64_t>& affected_samples) {
    const nlohmann::json& expected = direction == PatchDirection::Forward ? operation["before"] : operation["after"];
    const nlohmann::json& target = direction == PatchDirection::Forward ? operation["after"] : operation["before"];
    auto current = read_current_value(connection, operation);
    if (!current) return std::unexpected(current.error());
    if (*current != expected)
        return fail(ErrorCode::HistoryConflict, std::format("{} changed since: expected {}, found {}", operation.dump(),
                                                            expected.dump(), current->dump()));
    if (*current == target) return {};

    const auto kind = operation["op"].get<std::string>();
    if (kind == "field")
        return write_sample_field(connection, operation["sample"].get<std::int64_t>(),
                                  operation["field"].get<std::string>(), target, affected_samples);
    if (kind == "tag_link") return write_tag_link(connection, operation, target.get<bool>(), affected_samples);
    if (kind == "collection_link") return write_collection_link(connection, operation, target);

    const auto field = operation["field"].get<std::string>();
    if (kind == "tag_row" || kind == "category_row") {
        const bool is_tag = kind == "tag_row";
        const auto id = operation[is_tag ? "tag" : "category"].get<std::int64_t>();
        if (auto written = write_row_field(connection, is_tag ? "tags" : "categories", id, field, target); !written)
            return written;
        if (field == "name" || field == "parent_id") {
            auto members = samples_in_subtree(connection, is_tag ? Taxonomy::Tags : Taxonomy::Categories, id);
            if (!members) return std::unexpected(members.error());
            affected_samples.insert(members->begin(), members->end());
        }
        return {};
    }
    if (kind == "collection_row")
        return write_row_field(connection, "collections", operation["collection"].get<std::int64_t>(), field, target);
    return fail(ErrorCode::InvalidArgument, std::format("unknown patch operation '{}'", kind));
}

}  // namespace mso::catalog::detail
