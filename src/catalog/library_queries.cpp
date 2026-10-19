// Read-side queries: taxonomy trees, collections, folders, sample detail, review items.
#include <algorithm>
#include <format>
#include <map>

#include "mso/catalog/detail/catalog_internal.hpp"
#include "mso/catalog/library.hpp"

namespace mso::catalog {

namespace {

constexpr std::size_t kMaximumListLimit = 100'000;

std::optional<MusicalKey> key_from_columns(const std::optional<std::int64_t>& pitch_class,
                                           const std::optional<std::string>& mode) {
    if (!pitch_class || !mode || *pitch_class < 0 || *pitch_class > 11) return std::nullopt;
    return MusicalKey{static_cast<int>(*pitch_class), parse_key_mode(*mode).value_or(KeyMode::Unknown)};
}

std::int64_t clamp_limit(std::size_t limit) {
    return static_cast<std::int64_t>(std::min(limit, kMaximumListLimit));
}

/// Loads a tag or category tree with display paths built from parent links.
Result<std::vector<TaxonomyNode>> load_tree(sqlite::Connection& connection, detail::Taxonomy taxonomy) {
    const bool tags = taxonomy == detail::Taxonomy::Tags;
    auto statement = connection.prepare(
        tags ? "SELECT tag.id, tag.uuid, tag.parent_id, tag.name, (SELECT count(*) FROM sample_tags link "
               "JOIN samples sample ON sample.id=link.sample_id WHERE link.tag_id=tag.id AND sample.deleted=0), "
               "(SELECT canonical_tag_id FROM tag_aliases WHERE source_tag_id=tag.id) "
               "FROM tags tag WHERE tag.deleted=0 ORDER BY tag.name_key"
             : "SELECT category.id, category.uuid, category.parent_id, category.name, (SELECT count(*) FROM samples "
               "sample WHERE sample.category_id=category.id AND sample.deleted=0), NULL FROM categories category "
               "WHERE category.deleted=0 ORDER BY category.name_key");
    if (!statement) return std::unexpected(statement.error());
    std::vector<TaxonomyNode> nodes;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        auto uuid = detail::parse_stored_uuid(row.column_text(1), tags ? "tag" : "category");
        if (!uuid) return std::unexpected(uuid.error());
        nodes.push_back(TaxonomyNode{row.column_int64(0),
                                     *uuid,
                                     row.column_optional_int64(2),
                                     row.column_text(3),
                                     {},
                                     row.column_int64(4),
                                     row.column_optional_int64(5)});
        return {};
    });
    if (!walked) return std::unexpected(walked.error());

    std::map<std::int64_t, const TaxonomyNode*> by_id;
    for (const auto& node : nodes)
        by_id.emplace(node.id, &node);
    for (auto& node : nodes) {
        std::string path = node.name;
        auto parent = node.parent_id;
        for (std::size_t depth = 0; parent && depth < kMaxTaxonomyDepth; ++depth) {
            const auto found = by_id.find(*parent);
            if (found == by_id.end()) break;
            path = found->second->name + "/" + path;
            parent = found->second->parent_id;
        }
        node.path = std::move(path);
    }
    // Fold each path once, not inside the comparator.
    std::vector<std::pair<std::string, TaxonomyNode>> keyed;
    keyed.reserve(nodes.size());
    for (auto& node : nodes)
        keyed.emplace_back(fold_key(node.path), std::move(node));
    std::ranges::sort(keyed, {}, &std::pair<std::string, TaxonomyNode>::first);
    std::vector<TaxonomyNode> sorted;
    sorted.reserve(keyed.size());
    for (auto& [key, node] : keyed)
        sorted.push_back(std::move(node));
    return sorted;
}

}  // namespace

Result<std::int64_t> Library::ensure_tag(std::string_view path) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    std::vector<std::int64_t> created;
    auto id = detail::resolve_taxonomy_path(writer_, detail::Taxonomy::Tags, path, true, &created);
    if (!id) return std::unexpected(id.error());
    if (created.empty()) return *id;  // already existed: nothing committed, no revision
    auto revision = detail::advance_revision(
        writer_, {.kind = "tag_created", .structure_changed = true, .details = {{"tag_id", *id}}});
    if (!revision) return std::unexpected(revision.error());
    if (auto committed = commit_revision(transaction, *revision); !committed) return std::unexpected(committed.error());
    return *id;
}

Result<std::int64_t> Library::ensure_category(std::string_view path) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    std::vector<std::int64_t> created;
    auto id = detail::resolve_taxonomy_path(writer_, detail::Taxonomy::Categories, path, true, &created);
    if (!id) return std::unexpected(id.error());
    if (created.empty()) return *id;
    auto revision = detail::advance_revision(
        writer_, {.kind = "category_created", .structure_changed = true, .details = {{"category_id", *id}}});
    if (!revision) return std::unexpected(revision.error());
    if (auto committed = commit_revision(transaction, *revision); !committed) return std::unexpected(committed.error());
    return *id;
}

Result<std::vector<TaxonomyNode>> Library::list_tags() {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    return load_tree(reader->connection(), detail::Taxonomy::Tags);
}

Result<std::vector<TaxonomyNode>> Library::list_categories() {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    return load_tree(reader->connection(), detail::Taxonomy::Categories);
}

Result<std::vector<CollectionRecord>> Library::list_collections() {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto statement = reader->connection().prepare(
        "SELECT collection.id, collection.uuid, collection.name, collection.kind, collection.query_json, "
        "(SELECT count(*) FROM collection_items item JOIN samples sample ON sample.id=item.sample_id "
        "WHERE item.collection_id=collection.id AND sample.deleted=0) FROM collections collection "
        "WHERE collection.deleted=0 ORDER BY collection.name");
    if (!statement) return std::unexpected(statement.error());
    std::vector<CollectionRecord> collections;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        auto uuid = detail::parse_stored_uuid(row.column_text(1), "collection");
        if (!uuid) return std::unexpected(uuid.error());
        CollectionRecord record{row.column_int64(0),           *uuid,        row.column_text(2),
                                row.column_text(3) == "smart", std::nullopt, row.column_int64(5)};
        if (auto query = row.column_optional_text(4))
            record.query = detail::json_string_member(detail::parse_json(*query), "text");
        collections.push_back(std::move(record));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return collections;
}

Result<std::vector<FolderRecord>> Library::list_folders(std::int64_t root_id) {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto statement = reader->connection().prepare(
        "SELECT id, parent_id, relative_path FROM folders WHERE root_id=?1 ORDER BY path_key");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, root_id);
    std::vector<FolderRecord> folders;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        std::string path = row.column_text(2);
        const auto slash = path.rfind('/');
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        folders.push_back(
            FolderRecord{row.column_int64(0), root_id, row.column_optional_int64(1), std::move(path), std::move(name)});
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return folders;
}

Result<SampleDetail> Library::sample_detail(std::int64_t sample_id) {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto& connection = reader->connection();
    // Five statements, one snapshot: a concurrent commit can never produce a torn result.
    sqlite::ReadTransaction snapshot(connection);
    if (auto began = snapshot.begin(); !began) return std::unexpected(began.error());
    auto statement = connection.prepare(
        "SELECT sample.uuid, sample.display_name, sample.content_revision, sample.edit_revision, sample.favorite, "
        "sample.rating, sample.notes, sample.category_id, sample.bpm_override_state, sample.bpm_override_milli, "
        "sample.key_override_state, sample.key_override_class, sample.key_override_mode, effective.format, "
        "effective.codec, effective.sample_rate, effective.channels, effective.bit_depth, effective.source_frames, "
        "effective.duration_us, effective.duration_exact, effective.byte_size, effective.bpm_milli, "
        "effective.bpm_source, effective.bpm_confidence, effective.key_class, effective.key_mode, "
        "effective.key_source, effective.key_confidence, sample.created_at, sample.updated_at, sample.deleted "
        "FROM samples sample LEFT JOIN effective_metadata effective ON effective.sample_id=sample.id "
        "WHERE sample.id=?1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, sample_id);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::NotFound, "sample not found");

    SampleDetail result;
    const auto& row = *statement;
    result.id = sample_id;
    auto uuid = detail::parse_stored_uuid(row.column_text(0), "sample");
    if (!uuid) return std::unexpected(uuid.error());
    result.uuid = *uuid;
    result.display_name = row.column_text(1);
    result.content_revision = row.column_int64(2);
    result.edit_revision = row.column_int64(3);
    result.favorite = row.column_int64(4) != 0;
    result.rating = static_cast<int>(std::clamp<std::int64_t>(row.column_int64(5), 0, 5));
    result.notes = row.column_text(6);
    result.category_id = row.column_optional_int64(7);
    result.bpm_override = parse_override_state(row.column_text(8)).value_or(OverrideState::Automatic);
    result.bpm_override_milli = row.column_optional_int64(9);
    result.key_override = parse_override_state(row.column_text(10)).value_or(OverrideState::Automatic);
    result.key_override_value = key_from_columns(row.column_optional_int64(11), row.column_optional_text(12));
    result.format = row.column_optional_text(13).value_or("");
    result.codec = row.column_optional_text(14).value_or("");
    result.sample_rate = row.column_optional_int64(15);
    result.channels = row.column_optional_int64(16);
    result.bit_depth = row.column_optional_int64(17);
    result.frames = row.column_optional_int64(18);
    result.duration_us = row.column_optional_int64(19);
    result.duration_exact = row.column_int64(20) != 0;
    result.byte_size = row.column_optional_int64(21);
    result.bpm_milli = row.column_optional_int64(22);
    result.bpm_source = row.column_optional_text(23).value_or("");
    result.bpm_confidence = row.column_optional_double(24);
    result.key = key_from_columns(row.column_optional_int64(25), row.column_optional_text(26));
    result.key_source = row.column_optional_text(27).value_or("");
    result.key_confidence = row.column_optional_double(28);
    result.created_at = row.column_int64(29);
    result.updated_at = row.column_int64(30);
    result.deleted = row.column_int64(31) != 0;
    statement->reset();

    if (result.category_id) {
        auto path = detail::taxonomy_path(connection, detail::Taxonomy::Categories, *result.category_id);
        if (!path) return std::unexpected(path.error());
        result.category_path = *path;
    }

    auto tags = connection.prepare(
        "SELECT tag.id, tag.uuid, tag.name FROM sample_tags link JOIN tags tag ON tag.id=link.tag_id "
        "WHERE link.sample_id=?1 ORDER BY tag.name_key");
    if (!tags) return std::unexpected(tags.error());
    tags->bind(1, sample_id);
    auto walked = detail::for_each_row(*tags, [&](sqlite::PreparedStatement& tag) -> Result<void> {
        auto tag_uuid = detail::parse_stored_uuid(tag.column_text(1), "tag");
        if (!tag_uuid) return std::unexpected(tag_uuid.error());
        TaxonomyNode node{tag.column_int64(0), *tag_uuid, std::nullopt, tag.column_text(2), {}, 0, std::nullopt};
        auto path = detail::taxonomy_path(connection, detail::Taxonomy::Tags, node.id);
        if (!path) return std::unexpected(path.error());
        node.path = *path;
        result.tags.push_back(std::move(node));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());

    auto collections = connection.prepare(
        "SELECT collection.id, collection.uuid, collection.name FROM collection_items item "
        "JOIN collections collection ON collection.id=item.collection_id "
        "WHERE item.sample_id=?1 AND collection.deleted=0 ORDER BY collection.name");
    if (!collections) return std::unexpected(collections.error());
    collections->bind(1, sample_id);
    walked = detail::for_each_row(*collections, [&](sqlite::PreparedStatement& collection) -> Result<void> {
        auto collection_uuid = detail::parse_stored_uuid(collection.column_text(1), "collection");
        if (!collection_uuid) return std::unexpected(collection_uuid.error());
        result.collections.push_back(CollectionRecord{collection.column_int64(0), *collection_uuid,
                                                      collection.column_text(2), false, std::nullopt, 0});
        return {};
    });
    if (!walked) return std::unexpected(walked.error());

    auto locations = connection.prepare(
        "SELECT location.id, location.root_id, root.name, root.uuid, root.mode, location.relative_path, "
        "location.state, location.byte_size, location.mtime_ns, lower(hex(location.byte_hash)) FROM locations "
        "location JOIN roots root ON root.id=location.root_id WHERE location.sample_id=?1 AND "
        "location.state<>'retired' ORDER BY location.id");
    if (!locations) return std::unexpected(locations.error());
    locations->bind(1, sample_id);
    walked = detail::for_each_row(*locations, [&](sqlite::PreparedStatement& location) -> Result<void> {
        LocationRecord record;
        record.id = location.column_int64(0);
        record.root_id = location.column_int64(1);
        record.root_name = location.column_text(2);
        auto root_uuid = detail::parse_stored_uuid(location.column_text(3), "root");
        if (!root_uuid) return std::unexpected(root_uuid.error());
        record.root_uuid = *root_uuid;
        record.root_mode = parse_root_mode(location.column_text(4)).value_or(RootMode::Referenced);
        record.relative_path = location.column_text(5);
        record.state = parse_location_state(location.column_text(6)).value_or(LocationState::Unknown);
        record.byte_size = location.column_optional_int64(7);
        record.mtime_ns = location.column_optional_int64(8);
        if (!location.column_is_null(9)) record.byte_hash_hex = location.column_text(9);
        result.locations.push_back(std::move(record));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    if (!result.locations.empty()) {
        const auto& path = result.locations.front().relative_path;
        const auto slash = path.rfind('/');
        result.original_name = slash == std::string::npos ? path : path.substr(slash + 1);
    }

    auto observations = connection.prepare(
        "SELECT field, value_json, source, algorithm_version, confidence, content_revision, valid, created_at "
        "FROM metadata_observations WHERE sample_id=?1 ORDER BY id DESC LIMIT 100");
    if (!observations) return std::unexpected(observations.error());
    observations->bind(1, sample_id);
    walked = detail::for_each_row(*observations, [&](sqlite::PreparedStatement& observation) -> Result<void> {
        result.observations.push_back(ObservationRecord{
            observation.column_text(0), observation.column_text(1), observation.column_text(2),
            observation.column_optional_text(3), observation.column_optional_double(4), observation.column_int64(5),
            observation.column_int64(6) != 0, observation.column_int64(7)});
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return result;
}

Result<std::optional<std::int64_t>> Library::sample_id_by_uuid(const uuids::uuid& uuid) {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto statement = reader->connection().prepare("SELECT id FROM samples WHERE uuid=?1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, uuids::to_string(uuid));
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    auto id = *has_row ? std::optional<std::int64_t>(statement->column_int64(0)) : std::nullopt;
    statement->reset();
    return id;
}

Result<std::vector<ReviewItem>> Library::open_review_items(std::size_t limit) {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto statement = reader->connection().prepare(
        "SELECT id, uuid, kind, sample_id, evidence_json, state, created_at FROM review_items WHERE state='open' "
        "ORDER BY id DESC LIMIT ?1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, clamp_limit(limit));
    std::vector<ReviewItem> items;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        auto uuid = detail::parse_stored_uuid(row.column_text(1), "review item");
        if (!uuid) return std::unexpected(uuid.error());
        items.push_back(ReviewItem{row.column_int64(0), *uuid, row.column_text(2), row.column_optional_int64(3),
                                   row.column_text(4), row.column_text(5), row.column_int64(6)});
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return items;
}

Result<std::int64_t> Library::sample_count() {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    return reader->connection().query_int64("SELECT count(*) FROM samples WHERE deleted=0");
}

}  // namespace mso::catalog
