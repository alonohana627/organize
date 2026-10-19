#include "mso/catalog/detail/catalog_internal.hpp"

#include <format>

#include "mso/domain/core.hpp"

namespace mso::catalog::detail {

namespace {

constexpr std::int64_t kMinimumMilliBpm = 20'000;
constexpr std::int64_t kMaximumMilliBpm = 400'000;

/// Per-connection TEMP tables (disk-backed: temp_store=FILE). Their contents take part in the open
/// transaction, so a rollback also forgets what a failed mutation marked.
constexpr std::string_view kChangeTrackingTables = R"SQL(
CREATE TEMP TABLE IF NOT EXISTS changed_samples (
 sample_id INTEGER PRIMARY KEY, search_text INTEGER NOT NULL DEFAULT 0
);
CREATE TEMP TABLE IF NOT EXISTS projection_tag_paths (tag_id INTEGER PRIMARY KEY, path TEXT NOT NULL);
CREATE TEMP TABLE IF NOT EXISTS projection_category_paths (category_id INTEGER PRIMARY KEY, path TEXT NOT NULL);
)SQL";

/// Display paths of every tag linked to a queued sample (one recursive walk for the whole set).
constexpr std::string_view kFillTagPathsSql =
    "INSERT INTO temp.projection_tag_paths(tag_id, path) "
    "WITH RECURSIVE tag_chain(tag_id, ancestor_id, path, depth) AS ("
    " SELECT tag.id, tag.parent_id, tag.name, 0 FROM tags tag WHERE tag.id IN ("
    "  SELECT link.tag_id FROM sample_tags link JOIN temp.changed_samples queued "
    "  ON queued.sample_id=link.sample_id WHERE queued.search_text=1)"
    " UNION ALL SELECT chain.tag_id, parent.parent_id, parent.name || '/' || chain.path, chain.depth+1 "
    " FROM tag_chain chain JOIN tags parent ON parent.id=chain.ancestor_id WHERE chain.depth<16)"
    " SELECT tag_id, path FROM tag_chain WHERE ancestor_id IS NULL";

/// Display paths of every category assigned to a queued sample.
constexpr std::string_view kFillCategoryPathsSql =
    "INSERT INTO temp.projection_category_paths(category_id, path) "
    "WITH RECURSIVE category_chain(category_id, ancestor_id, path, depth) AS ("
    " SELECT category.id, category.parent_id, category.name, 0 FROM categories category WHERE category.id IN ("
    "  SELECT sample.category_id FROM samples sample JOIN temp.changed_samples queued "
    "  ON queued.sample_id=sample.id WHERE queued.search_text=1)"
    " UNION ALL SELECT chain.category_id, parent.parent_id, parent.name || '/' || chain.path, chain.depth+1 "
    " FROM category_chain chain JOIN categories parent ON parent.id=chain.ancestor_id WHERE chain.depth<16)"
    " SELECT category_id, path FROM category_chain WHERE ancestor_id IS NULL";

/// Upserts the projection row of every queued live sample in one statement.
constexpr std::string_view kUpsertProjectionSql =
    "INSERT INTO sample_search(sample_id, name, path, tags, category, notes, embedded) "
    "SELECT sample.id, sample.display_name, "
    " COALESCE((SELECT group_concat(root.name || '/' || location.relative_path, ' | ' ORDER BY location.id) "
    "  FROM locations location JOIN roots root ON root.id=location.root_id "
    "  WHERE location.sample_id=sample.id AND location.state<>'retired'), ''), "
    " COALESCE((SELECT group_concat(tag_path.path, ' | ' ORDER BY link.tag_id) "
    "  FROM sample_tags link JOIN temp.projection_tag_paths tag_path ON tag_path.tag_id=link.tag_id "
    "  WHERE link.sample_id=sample.id), ''), "
    " COALESCE((SELECT category_path.path FROM temp.projection_category_paths category_path "
    "  WHERE category_path.category_id=sample.category_id), ''), "
    " sample.notes, "
    " COALESCE((SELECT CASE WHEN json_type(observation.value_json)='text' "
    "   THEN json_extract(observation.value_json, '$') END "
    "  FROM metadata_observations observation WHERE observation.sample_id=sample.id "
    "  AND observation.field='embedded_text' AND observation.valid=1 "
    "  AND observation.content_revision=sample.content_revision ORDER BY observation.id DESC LIMIT 1), '') "
    "FROM samples sample JOIN temp.changed_samples queued ON queued.sample_id=sample.id "
    "WHERE queued.search_text=1 AND sample.deleted=0 "
    "ON CONFLICT(sample_id) DO UPDATE SET name=excluded.name, path=excluded.path, tags=excluded.tags, "
    "category=excluded.category, notes=excluded.notes, embedded=excluded.embedded";

/// Removes projection rows of queued samples that are tombstoned or gone.
constexpr std::string_view kRemoveProjectionSql =
    "DELETE FROM sample_search WHERE sample_id IN (SELECT sample_id FROM temp.changed_samples WHERE search_text=1) "
    "AND NOT EXISTS(SELECT 1 FROM samples sample WHERE sample.id=sample_search.sample_id AND sample.deleted=0)";

Result<void> run_statement(sqlite::Connection& connection, std::string_view sql) {
    auto statement = connection.prepare(sql);
    if (!statement) return std::unexpected(statement.error());
    return statement->run();
}

/// Latest valid observation of `field` for the sample's current content revision, preferring the
/// highest-precedence source (accepted suggestion > container > analysis > filename).
struct Candidate {
    nlohmann::json value;
    std::string source;
    std::optional<double> confidence;
};

Result<std::optional<Candidate>> best_observation(sqlite::Connection& connection, std::int64_t sample_id,
                                                  std::string_view field) {
    auto statement = connection.prepare(
        "SELECT observation.value_json, observation.source, observation.confidence FROM metadata_observations "
        "observation JOIN samples sample ON sample.id=observation.sample_id WHERE observation.sample_id=?1 "
        "AND observation.field=?2 AND observation.valid=1 AND observation.content_revision=sample.content_revision "
        "ORDER BY CASE observation.source WHEN 'accepted_suggestion' THEN 0 WHEN 'container' THEN 1 "
        "WHEN 'analysis' THEN 2 ELSE 3 END, observation.id DESC LIMIT 1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, sample_id).bind(2, field);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return std::optional<Candidate>{};
    auto value = json_fields::parse_json(statement->column_text(0));
    auto candidate = Candidate{std::move(value), statement->column_text(1), statement->column_optional_double(2)};
    statement->reset();
    if (candidate.value.is_discarded() || candidate.value.is_null()) return std::optional<Candidate>{};
    return std::optional<Candidate>(std::move(candidate));
}

}  // namespace

Result<void> for_each_row(sqlite::PreparedStatement& statement,
                          const std::function<Result<void>(sqlite::PreparedStatement&)>& row) {
    while (true) {
        auto has_row = statement.next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) {
            statement.reset();
            return {};
        }
        if (auto handled = row(statement); !handled) {
            statement.reset();
            return handled;
        }
    }
}

Result<uuids::uuid> parse_stored_uuid(std::string_view text, std::string_view what) {
    auto uuid = uuids::uuid::from_string(text);
    if (!uuid) return fail(ErrorCode::DbCorrupt, std::format("stored {} UUID '{}' is malformed", what, text));
    return *uuid;
}

// ---------------------------------------------------------------- change tracking and outbox

Result<void> enforce_outbox_cap(sqlite::Connection& connection, std::int64_t maximum_bytes) {
    auto cutoff = connection.prepare(
        "SELECT revision FROM (SELECT revision, sum(length(payload_json)) OVER (ORDER BY revision DESC) AS newer_bytes "
        "FROM catalog_outbox) WHERE newer_bytes>?1 ORDER BY revision DESC LIMIT 1");
    if (!cutoff) return std::unexpected(cutoff.error());
    cutoff->bind(1, maximum_bytes);
    auto has_row = cutoff->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return {};
    const std::int64_t oldest_dropped_through = cutoff->column_int64(0);
    cutoff->reset();
    auto prune = connection.prepare("DELETE FROM catalog_outbox WHERE revision<=?1");
    if (!prune) return std::unexpected(prune.error());
    prune->bind(1, oldest_dropped_through);
    return prune->run();
}

Result<void> prepare_change_tracking(sqlite::Connection& connection) {
    return connection.execute_script(kChangeTrackingTables);
}

Result<void> mark_sample_changed(sqlite::Connection& connection, std::int64_t sample_id, bool search_text_changed) {
    auto mark = connection.prepare(
        "INSERT INTO temp.changed_samples(sample_id, search_text) VALUES(?1, ?2) "
        "ON CONFLICT(sample_id) DO UPDATE SET search_text=max(search_text, excluded.search_text)");
    if (!mark) return std::unexpected(mark.error());
    mark->bind(1, sample_id).bind(2, search_text_changed ? 1 : 0);
    return mark->run();
}

Result<void> mark_sample_list_changed(sqlite::Connection& connection, std::span<const std::int64_t> sample_ids,
                                      bool search_text_changed) {
    if (sample_ids.empty()) return {};
    auto mark = connection.prepare(
        "INSERT INTO temp.changed_samples(sample_id, search_text) SELECT DISTINCT value, ?2 FROM json_each(?1) "
        "WHERE type='integer' ON CONFLICT(sample_id) DO UPDATE SET search_text=max(search_text, excluded.search_text)");
    if (!mark) return std::unexpected(mark.error());
    mark->bind(1,
               json_fields::dump_json(nlohmann::json(std::vector<std::int64_t>(sample_ids.begin(), sample_ids.end()))));
    mark->bind(2, search_text_changed ? 1 : 0);
    return mark->run();
}

Result<void> mark_samples_changed(sqlite::Connection& connection, std::string_view select_sample_ids_sql,
                                  std::span<const std::int64_t> parameters, bool search_text_changed) {
    auto mark = connection.prepare(
        std::format("INSERT INTO temp.changed_samples(sample_id, search_text) SELECT DISTINCT candidate.sample_id, {} "
                    "FROM ({}) AS candidate WHERE candidate.sample_id IS NOT NULL "
                    "ON CONFLICT(sample_id) DO UPDATE SET search_text=max(search_text, excluded.search_text)",
                    search_text_changed ? 1 : 0, select_sample_ids_sql));
    if (!mark) return std::unexpected(mark.error());
    int index = 1;
    for (const auto parameter : parameters)
        mark->bind(index++, parameter);
    return mark->run();
}

Result<void> refresh_search_projection(sqlite::Connection& connection) {
    auto pending = connection.query_int64("SELECT EXISTS(SELECT 1 FROM temp.changed_samples WHERE search_text=1)");
    if (!pending) return std::unexpected(pending.error());
    if (*pending == 0) return {};
    for (const auto sql : {std::string_view("DELETE FROM temp.projection_tag_paths"),
                           std::string_view("DELETE FROM temp.projection_category_paths"), kFillTagPathsSql,
                           kFillCategoryPathsSql, kUpsertProjectionSql, kRemoveProjectionSql,
                           std::string_view("UPDATE temp.changed_samples SET search_text=0 WHERE search_text=1")}) {
        if (auto ran = run_statement(connection, sql); !ran) return ran;
    }
    return {};
}

Result<std::int64_t> advance_revision(sqlite::Connection& connection, const ChangeEnvelope& envelope) {
    if (auto projected = refresh_search_projection(connection); !projected) return std::unexpected(projected.error());

    auto bump = connection.prepare(
        "UPDATE library_state SET catalog_revision=catalog_revision+1 WHERE singleton=1 RETURNING catalog_revision");
    if (!bump) return std::unexpected(bump.error());
    auto bumped = bump->next_row();
    if (!bumped) return std::unexpected(bumped.error());
    if (!*bumped) return fail(ErrorCode::DbCorrupt, "library_state row is missing");
    const std::int64_t revision = bump->column_int64(0);
    bump->reset();

    auto changed_count = connection.query_int64("SELECT count(*) FROM temp.changed_samples");
    if (!changed_count) return std::unexpected(changed_count.error());
    nlohmann::json payload = envelope.details.is_object() ? envelope.details : nlohmann::json::object();
    payload["kind"] = envelope.kind;
    payload["structure"] = envelope.structure_changed;
    nlohmann::json samples = nlohmann::json::array();
    if (*changed_count > kMaximumEnvelopeSamples) {
        payload["all_samples"] = true;
    } else if (*changed_count > 0) {
        auto changed = connection.prepare("SELECT sample_id FROM temp.changed_samples ORDER BY sample_id");
        if (!changed) return std::unexpected(changed.error());
        auto walked = for_each_row(*changed, [&](sqlite::PreparedStatement& row) -> Result<void> {
            samples.push_back(row.column_int64(0));
            return {};
        });
        if (!walked) return std::unexpected(walked.error());
    }
    payload["samples"] = std::move(samples);

    auto insert = connection.prepare(
        "INSERT INTO catalog_outbox(revision,envelope_version,payload_json,created_at) VALUES(?1,?2,?3,?4)");
    if (!insert) return std::unexpected(insert.error());
    insert->bind(1, revision)
        .bind(2, kOutboxEnvelopeVersion)
        .bind(3, json_fields::dump_json(payload))
        .bind(4, now_unix_ms());
    if (auto inserted = insert->run(); !inserted) return std::unexpected(inserted.error());
    if (auto cleared = run_statement(connection, "DELETE FROM temp.changed_samples"); !cleared)
        return std::unexpected(cleared.error());
    if (revision % kOutboxSizeCheckInterval == 0)
        if (auto capped = enforce_outbox_cap(connection); !capped) return std::unexpected(capped.error());
    return revision;
}

// ---------------------------------------------------------------- taxonomy

std::string_view taxonomy_table(Taxonomy taxonomy) noexcept {
    return taxonomy == Taxonomy::Tags ? "tags" : "categories";
}

Result<std::int64_t> resolve_taxonomy_path(sqlite::Connection& connection, Taxonomy taxonomy, std::string_view path,
                                           bool create, std::vector<std::int64_t>* created) {
    auto segments = split_taxonomy_path(path);
    if (!segments) return std::unexpected(segments.error());
    const auto table = taxonomy_table(taxonomy);
    // A merged (tombstoned) tag still resolves by its old name, through its alias to the canonical tag.
    auto find = connection.prepare(
        taxonomy == Taxonomy::Tags
            ? "SELECT id FROM tags WHERE COALESCE(parent_id,0)=COALESCE(?1,0) AND name_key=?2 AND "
              "(deleted=0 OR id IN (SELECT source_tag_id FROM tag_aliases)) ORDER BY deleted LIMIT 1"
            : "SELECT id FROM categories WHERE COALESCE(parent_id,0)=COALESCE(?1,0) AND name_key=?2 AND deleted=0");
    auto insert = connection.prepare(
        std::format("INSERT INTO {}(uuid,parent_id,name,name_key) VALUES(new_uuid(),?1,?2,?3)", table));
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
        insert->bind(1, parent).bind(2, segment).bind(3, key);
        if (auto inserted = insert->run(); !inserted) return std::unexpected(inserted.error());
        parent = connection.last_insert_rowid();
        if (created) created->push_back(*parent);
    }
    return *parent;
}

Result<std::string> taxonomy_path(sqlite::Connection& connection, Taxonomy taxonomy, std::int64_t id) {
    auto statement = connection.prepare(
        std::format("WITH RECURSIVE chain(id,parent_id,name,depth) AS ("
                    " SELECT id,parent_id,name,0 FROM {0} WHERE id=?1"
                    " UNION ALL SELECT node.id,node.parent_id,node.name,chain.depth+1 FROM {0} node JOIN chain "
                    " ON node.id=chain.parent_id WHERE chain.depth<16)"
                    " SELECT name FROM chain ORDER BY depth DESC",
                    taxonomy_table(taxonomy)));
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
        if (!*has_row) {
            statement->reset();
            return current;
        }
        current = statement->column_int64(0);
        statement->reset();
    }
    return fail(ErrorCode::DbCorrupt, "tag alias chain too long or cyclic");
}

Result<void> add_review_item(sqlite::Connection& connection, std::string_view kind,
                             std::optional<std::int64_t> sample_id, const nlohmann::json& evidence) {
    auto insert = connection.prepare(
        "INSERT INTO review_items(uuid,kind,sample_id,evidence_json,state,created_at) "
        "VALUES(new_uuid(),?1,?2,?3,'open',?4)");
    if (!insert) return std::unexpected(insert.error());
    insert->bind(1, kind).bind(2, sample_id).bind(3, json_fields::dump_json(evidence)).bind(4, now_unix_ms());
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

    // Observations come from probes, filenames and workers: anything out of range is ignored rather
    // than allowed to violate the effective_metadata CHECK constraints.
    auto valid_confidence = [](const std::optional<double>& confidence) {
        return !confidence || (*confidence >= 0.0 && *confidence <= 1.0);
    };
    if (bpm_state == "auto") {
        auto candidate = best_observation(connection, sample_id, "bpm_milli");
        if (!candidate) return std::unexpected(candidate.error());
        if (*candidate) {
            const auto value = json_fields::integer_value((*candidate)->value);
            if (value && *value >= kMinimumMilliBpm && *value <= kMaximumMilliBpm &&
                valid_confidence((*candidate)->confidence)) {
                bpm = *value;
                bpm_source = (*candidate)->source;
                bpm_confidence = (*candidate)->confidence;
            }
        }
    }
    if (key_state == "auto") {
        auto candidate = best_observation(connection, sample_id, "key");
        if (!candidate) return std::unexpected(candidate.error());
        if (*candidate) {
            const auto pitch_class = json_fields::integer_field((*candidate)->value, "pitch_class");
            const auto mode = json_fields::string_field((*candidate)->value, "mode");
            if (pitch_class && *pitch_class >= 0 && *pitch_class <= 11 && mode && parse_key_mode(*mode) &&
                valid_confidence((*candidate)->confidence)) {
                key_class = *pitch_class;
                key_mode = *mode;
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

// ---------------------------------------------------------------- analysis queue reachability

Result<void> block_unreachable_jobs(sqlite::Connection& connection, std::int64_t root_id) {
    auto block = connection.prepare(std::format(
        "UPDATE jobs SET state='blocked', error_code='SOURCE_OFFLINE', lease_expires_at=NULL "
        "WHERE state IN ('queued','interrupted') AND sample_id IN (SELECT sample_id FROM locations WHERE root_id=?1) "
        "AND NOT {}",
        kJobSampleReachableSql));
    if (!block) return std::unexpected(block.error());
    block->bind(1, root_id);
    return block->run();
}

Result<void> unblock_reachable_jobs(sqlite::Connection& connection, std::int64_t root_id) {
    // A job of a paused run returns as `paused` (Resume queues it), never into the lease index.
    auto unblock = connection.prepare(std::format(
        "UPDATE jobs SET state=CASE WHEN (SELECT run.state FROM analysis_runs run WHERE run.id=jobs.run_id)='paused' "
        "THEN 'paused' ELSE 'queued' END, error_code=NULL, lease_expires_at=NULL WHERE state='blocked' "
        "AND sample_id IN (SELECT sample_id FROM locations WHERE root_id=?1 AND state='present') AND {}",
        kJobSampleReachableSql));
    if (!unblock) return std::unexpected(unblock.error());
    unblock->bind(1, root_id);
    return unblock->run();
}

}  // namespace mso::catalog::detail
