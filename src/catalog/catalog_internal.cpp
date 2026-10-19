#include "mso/catalog/catalog_internal.hpp"

#include <format>

#include "mso/domain/core.hpp"

namespace mso::catalog::detail {

namespace {

constexpr std::size_t kMaxTaxonomyDepth = 16;
constexpr std::size_t kMaxTaxonomyNameCodePoints = 128;

std::string_view table_name(Taxonomy taxonomy) {
    return taxonomy == Taxonomy::Tags ? "tags" : "categories";
}

/// Joins the display paths of a sample's (canonical) tags with " | ".
Result<std::string> joined_tag_paths(sqlite::Connection& connection, std::int64_t sample_id) {
    auto statement = connection.prepare("SELECT tag_id FROM sample_tags WHERE sample_id=?1 ORDER BY tag_id");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, sample_id);
    std::string joined;
    auto walked = for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        auto path = taxonomy_path(connection, Taxonomy::Tags, row.column_int64(0));
        if (!path) return std::unexpected(path.error());
        if (!joined.empty()) joined += " | ";
        joined += *path;
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return joined;
}

/// Joins a sample's live location paths ("Root/sub/file.wav") with " | ".
Result<std::string> joined_location_paths(sqlite::Connection& connection, std::int64_t sample_id) {
    auto statement = connection.prepare(
        "SELECT r.name, l.relative_path FROM locations l JOIN roots r ON r.id=l.root_id "
        "WHERE l.sample_id=?1 AND l.state<>'retired' ORDER BY l.id");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, sample_id);
    std::string joined;
    auto walked = for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        if (!joined.empty()) joined += " | ";
        joined += row.column_text(0) + "/" + row.column_text(1);
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return joined;
}

/// Latest valid observation value of `field` for the sample's current content revision.
Result<std::optional<std::string>> current_observation_text(sqlite::Connection& connection, std::int64_t sample_id,
                                                            std::string_view field) {
    auto statement = connection.prepare(
        "SELECT o.value_json FROM metadata_observations o JOIN samples s ON s.id=o.sample_id "
        "WHERE o.sample_id=?1 AND o.field=?2 AND o.valid=1 AND o.content_revision=s.content_revision "
        "ORDER BY o.id DESC LIMIT 1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, sample_id).bind(2, field);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return std::optional<std::string>{};
    auto value = nlohmann::json::parse(statement->column_text(0), nullptr, false);
    if (value.is_string()) return std::optional<std::string>(value.get<std::string>());
    return std::optional<std::string>{};
}

/// One provenanced candidate for an effective musical fact.
struct Candidate {
    nlohmann::json value;
    std::string source;
    std::optional<double> confidence;
};

/// Highest-precedence valid observation of `field` (container > analysis > filename).
Result<std::optional<Candidate>> best_observation(sqlite::Connection& connection, std::int64_t sample_id,
                                                  std::string_view field) {
    auto statement = connection.prepare(
        "SELECT o.value_json, o.source, o.confidence FROM metadata_observations o JOIN samples s ON s.id=o.sample_id "
        "WHERE o.sample_id=?1 AND o.field=?2 AND o.valid=1 AND o.content_revision=s.content_revision "
        "ORDER BY CASE o.source WHEN 'accepted_suggestion' THEN 0 WHEN 'container' THEN 1 WHEN 'analysis' THEN 2 "
        "ELSE 3 END, o.id DESC LIMIT 1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, sample_id).bind(2, field);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return std::optional<Candidate>{};
    auto value = nlohmann::json::parse(statement->column_text(0), nullptr, false);
    if (value.is_discarded() || value.is_null()) return std::optional<Candidate>{};
    return std::optional<Candidate>(Candidate{value, statement->column_text(1), statement->column_optional_double(2)});
}

}  // namespace

Result<void> for_each_row(sqlite::PreparedStatement& statement,
                          const std::function<Result<void>(sqlite::PreparedStatement&)>& row) {
    while (true) {
        auto has_row = statement.next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) return {};
        if (auto handled = row(statement); !handled) {
            statement.reset();
            return handled;
        }
    }
}

Result<std::int64_t> advance_revision(sqlite::Connection& connection, const nlohmann::json& envelope) {
    if (auto bumped =
            connection.execute_script("UPDATE library_state SET catalog_revision=catalog_revision+1 WHERE singleton=1");
        !bumped)
        return std::unexpected(bumped.error());
    auto revision = connection.query_int64("SELECT catalog_revision FROM library_state WHERE singleton=1");
    if (!revision) return std::unexpected(revision.error());
    auto insert = connection.prepare(
        "INSERT INTO catalog_outbox(revision,envelope_version,payload_json,created_at) VALUES(?1,?2,?3,?4)");
    if (!insert) return std::unexpected(insert.error());
    insert->bind(1, *revision).bind(2, kOutboxEnvelopeVersion).bind(3, envelope.dump()).bind(4, now_unix_ms());
    if (auto inserted = insert->run(); !inserted) return std::unexpected(inserted.error());
    return *revision;
}

Result<void> refresh_search_projection(sqlite::Connection& connection, const std::set<std::int64_t>& sample_ids) {
    auto select_sample =
        connection.prepare("SELECT display_name, notes, category_id, deleted FROM samples WHERE id=?1");
    auto remove = connection.prepare("DELETE FROM sample_search WHERE sample_id=?1");
    auto upsert = connection.prepare(
        "INSERT INTO sample_search(sample_id,name,path,tags,category,notes,embedded) VALUES(?1,?2,?3,?4,?5,?6,?7) "
        "ON CONFLICT(sample_id) DO UPDATE SET name=excluded.name, path=excluded.path, tags=excluded.tags, "
        "category=excluded.category, notes=excluded.notes, embedded=excluded.embedded");
    if (!select_sample) return std::unexpected(select_sample.error());
    if (!remove) return std::unexpected(remove.error());
    if (!upsert) return std::unexpected(upsert.error());

    for (const std::int64_t sample_id : sample_ids) {
        select_sample->bind(1, sample_id);
        auto has_row = select_sample->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row || select_sample->column_int64(3) != 0) {
            select_sample->reset();
            remove->bind(1, sample_id);
            if (auto removed = remove->run(); !removed) return removed;
            continue;
        }
        const std::string display_name = select_sample->column_text(0);
        const std::string notes = select_sample->column_text(1);
        const auto category_id = select_sample->column_optional_int64(2);
        select_sample->reset();

        auto tags = joined_tag_paths(connection, sample_id);
        if (!tags) return std::unexpected(tags.error());
        auto paths = joined_location_paths(connection, sample_id);
        if (!paths) return std::unexpected(paths.error());
        std::string category;
        if (category_id) {
            auto path = taxonomy_path(connection, Taxonomy::Categories, *category_id);
            if (!path) return std::unexpected(path.error());
            category = *path;
        }
        auto embedded = current_observation_text(connection, sample_id, "embedded_text");
        if (!embedded) return std::unexpected(embedded.error());

        upsert->bind(1, sample_id).bind(2, display_name).bind(3, *paths).bind(4, *tags).bind(5, category);
        upsert->bind(6, notes).bind(7, embedded->value_or(""));
        if (auto written = upsert->run(); !written) return written;
    }
    return {};
}

Result<std::vector<std::string>> split_taxonomy_path(std::string_view path) {
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        std::string segment = trim(path.substr(start, end - start));
        if (segment.empty()) return fail(ErrorCode::InvalidArgument, "empty tag or category name in path");
        if (count_code_points(segment) > kMaxTaxonomyNameCodePoints)
            return fail(ErrorCode::InvalidArgument, "tag or category name longer than 128 characters");
        if (!is_valid_utf8(segment)) return fail(ErrorCode::InvalidArgument, "tag or category name is not valid UTF-8");
        segments.push_back(std::move(segment));
        start = end + 1;
    }
    if (segments.size() > kMaxTaxonomyDepth) return fail(ErrorCode::InvalidArgument, "hierarchy deeper than 16 levels");
    return segments;
}

Result<std::int64_t> resolve_taxonomy_path(sqlite::Connection& connection, Taxonomy taxonomy, std::string_view path,
                                           bool create) {
    auto segments = split_taxonomy_path(path);
    if (!segments) return std::unexpected(segments.error());
    const auto table = table_name(taxonomy);
    // A merged (tombstoned) tag still resolves by its old name, through its alias to the canonical tag.
    auto find = connection.prepare(
        taxonomy == Taxonomy::Tags
            ? "SELECT id FROM tags WHERE COALESCE(parent_id,0)=COALESCE(?1,0) AND name_key=?2 AND "
              "(deleted=0 OR id IN (SELECT source_tag_id FROM tag_aliases)) ORDER BY deleted LIMIT 1"
            : "SELECT id FROM categories WHERE COALESCE(parent_id,0)=COALESCE(?1,0) AND name_key=?2 AND deleted=0");
    auto insert =
        connection.prepare(std::format("INSERT INTO {}(uuid,parent_id,name,name_key) VALUES(?1,?2,?3,?4)", table));
    if (!find) return std::unexpected(find.error());
    if (!insert) return std::unexpected(insert.error());

    std::optional<std::int64_t> parent;
    for (const auto& segment : *segments) {
        const std::string key = fold_key(segment);
        find->bind(1, parent).bind(2, key);
        auto has_row = find->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (*has_row) {
            std::int64_t id = find->column_int64(0);
            find->reset();
            if (taxonomy == Taxonomy::Tags) {
                auto canonical = canonical_tag(connection, id);
                if (!canonical) return std::unexpected(canonical.error());
                id = *canonical;
            }
            parent = id;
            continue;
        }
        find->reset();
        if (!create) return fail(ErrorCode::NotFound, std::format("no {} named '{}'", table, path));
        insert->bind(1, uuids::to_string(new_uuid())).bind(2, parent).bind(3, segment).bind(4, key);
        if (auto inserted = insert->run(); !inserted) return std::unexpected(inserted.error());
        parent = connection.last_insert_rowid();
    }
    return *parent;
}

Result<std::string> taxonomy_path(sqlite::Connection& connection, Taxonomy taxonomy, std::int64_t id) {
    auto statement =
        connection.prepare(std::format("WITH RECURSIVE chain(id,parent_id,name,depth) AS ("
                                       " SELECT id,parent_id,name,0 FROM {0} WHERE id=?1"
                                       " UNION ALL SELECT t.id,t.parent_id,t.name,c.depth+1 FROM {0} t JOIN chain c ON "
                                       "t.id=c.parent_id WHERE c.depth<16)"
                                       " SELECT name FROM chain ORDER BY depth DESC",
                                       table_name(taxonomy)));
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, id);
    std::string path;
    auto walked = for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        if (!path.empty()) path += '/';
        path += row.column_text(0);
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return path;
}

Result<std::int64_t> canonical_tag(sqlite::Connection& connection, std::int64_t tag_id) {
    auto statement = connection.prepare("SELECT canonical_tag_id FROM tag_aliases WHERE source_tag_id=?1");
    if (!statement) return std::unexpected(statement.error());
    std::int64_t current = tag_id;
    for (std::size_t hop = 0; hop <= kMaxTaxonomyDepth; ++hop) {
        statement->bind(1, current);
        auto has_row = statement->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) return current;
        current = statement->column_int64(0);
        statement->reset();
    }
    return fail(ErrorCode::Internal, "tag alias chain too long or cyclic");
}

Result<void> add_review_item(sqlite::Connection& connection, std::string_view kind,
                             std::optional<std::int64_t> sample_id, const nlohmann::json& evidence) {
    auto insert = connection.prepare(
        "INSERT INTO review_items(uuid,kind,sample_id,evidence_json,state,created_at) VALUES(?1,?2,?3,?4,'open',?5)");
    if (!insert) return std::unexpected(insert.error());
    insert->bind(1, uuids::to_string(new_uuid())).bind(2, kind).bind(3, sample_id).bind(4, evidence.dump());
    insert->bind(5, now_unix_ms());
    return insert->run();
}

Result<void> recompute_effective_musical_facts(sqlite::Connection& connection, std::int64_t sample_id) {
    auto overrides = connection.prepare(
        "SELECT bpm_override_state, bpm_override_milli, key_override_state, key_override_class, key_override_mode "
        "FROM samples WHERE id=?1");
    if (!overrides) return std::unexpected(overrides.error());
    overrides->bind(1, sample_id);
    auto has_row = overrides->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::NotFound, "sample not found");

    std::optional<std::int64_t> bpm;
    std::optional<std::string> bpm_source;
    std::optional<double> bpm_confidence;
    const std::string bpm_state = overrides->column_text(0);
    if (bpm_state == "value") {
        bpm = overrides->column_int64(1);
        bpm_source = "user";
    } else if (bpm_state == "unknown") {
        bpm_source = "user_unknown";
    }

    std::optional<std::int64_t> key_class;
    std::optional<std::string> key_mode, key_source;
    std::optional<double> key_confidence;
    const std::string key_state = overrides->column_text(2);
    if (key_state == "value") {
        key_class = overrides->column_int64(3);
        key_mode = overrides->column_text(4);
        key_source = "user";
    } else if (key_state == "unknown") {
        key_source = "user_unknown";
    }
    overrides->reset();

    if (bpm_state == "auto") {
        auto candidate = best_observation(connection, sample_id, "bpm_milli");
        if (!candidate) return std::unexpected(candidate.error());
        if (*candidate && (*candidate)->value.is_number_integer()) {
            bpm = (*candidate)->value.get<std::int64_t>();
            bpm_source = (*candidate)->source;
            bpm_confidence = (*candidate)->confidence;
        }
    }
    if (key_state == "auto") {
        auto candidate = best_observation(connection, sample_id, "key");
        if (!candidate) return std::unexpected(candidate.error());
        if (*candidate && (*candidate)->value.is_object()) {
            const auto& value = (*candidate)->value;
            if (value.contains("pitch_class") && value.contains("mode")) {
                key_class = value["pitch_class"].get<std::int64_t>();
                key_mode = value["mode"].get<std::string>();
                key_source = (*candidate)->source;
                key_confidence = (*candidate)->confidence;
            }
        }
    }

    auto update = connection.prepare(
        "UPDATE effective_metadata SET bpm_milli=?2, bpm_source=?3, bpm_confidence=?4, key_class=?5, key_mode=?6, "
        "key_source=?7, key_confidence=?8 WHERE sample_id=?1");
    if (!update) return std::unexpected(update.error());
    update->bind(1, sample_id).bind(2, bpm).bind(3, bpm_source).bind(4, bpm_confidence);
    update->bind(5, key_class).bind(6, key_mode).bind(7, key_source).bind(8, key_confidence);
    return update->run();
}

}  // namespace mso::catalog::detail
