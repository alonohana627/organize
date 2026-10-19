// Read-side queries: taxonomy trees, collections, folders, sample detail, review items.
#include <format>

#include <map>

#include "mso/catalog/catalog_internal.hpp"
#include "mso/catalog/library.hpp"

namespace mso::catalog {

namespace {

std::optional<MusicalKey> key_from_columns(const std::optional<std::int64_t>& pitch_class,
                                           const std::optional<std::string>& mode) {
    if (!pitch_class || !mode) return std::nullopt;
    return MusicalKey{static_cast<int>(*pitch_class), parse_key_mode(*mode).value_or(KeyMode::Unknown)};
}

/// Loads a tag or category tree with display paths built from parent links.
Result<std::vector<TaxonomyNode>> load_tree(sqlite::Connection& connection, detail::Taxonomy taxonomy) {
    const bool tags = taxonomy == detail::Taxonomy::Tags;
    auto statement = connection.prepare(
        tags ? "SELECT t.id, t.uuid, t.parent_id, t.name, (SELECT count(*) FROM sample_tags st JOIN samples s "
               "ON s.id=st.sample_id WHERE st.tag_id=t.id AND s.deleted=0), "
               "(SELECT canonical_tag_id FROM tag_aliases WHERE source_tag_id=t.id) "
               "FROM tags t WHERE t.deleted=0 ORDER BY t.name_key"
             : "SELECT c.id, c.uuid, c.parent_id, c.name, (SELECT count(*) FROM samples s WHERE s.category_id=c.id "
               "AND s.deleted=0), NULL FROM categories c WHERE c.deleted=0 ORDER BY c.name_key");
    if (!statement) return std::unexpected(statement.error());
    std::vector<TaxonomyNode> nodes;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        nodes.push_back(TaxonomyNode{row.column_int64(0),
                                     uuids::uuid::from_string(row.column_text(1)).value_or(uuids::uuid()),
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
        for (int depth = 0; parent && depth < 16; ++depth) {
            const auto found = by_id.find(*parent);
            if (found == by_id.end()) break;
            path = found->second->name + "/" + path;
            parent = found->second->parent_id;
        }
        node.path = std::move(path);
    }
    std::ranges::sort(nodes,
                      [](const TaxonomyNode& a, const TaxonomyNode& b) { return fold_key(a.path) < fold_key(b.path); });
    return nodes;
}

}  // namespace

Result<std::int64_t> Library::ensure_tag(std::string_view path) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    auto id = detail::resolve_taxonomy_path(writer_, detail::Taxonomy::Tags, path, true);
    if (!id) return std::unexpected(id.error());
    if (auto revision = detail::advance_revision(writer_, nlohmann::json{{"kind", "tag_created"}, {"tag_id", *id}});
        !revision)
        return std::unexpected(revision.error());
    if (auto committed = transaction.commit(); !committed) return std::unexpected(committed.error());
    return *id;
}

Result<std::int64_t> Library::ensure_category(std::string_view path) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    auto id = detail::resolve_taxonomy_path(writer_, detail::Taxonomy::Categories, path, true);
    if (!id) return std::unexpected(id.error());
    if (auto revision =
            detail::advance_revision(writer_, nlohmann::json{{"kind", "category_created"}, {"category_id", *id}});
        !revision)
        return std::unexpected(revision.error());
    if (auto committed = transaction.commit(); !committed) return std::unexpected(committed.error());
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
        "SELECT c.id, c.uuid, c.name, c.kind, c.query_json, (SELECT count(*) FROM collection_items i JOIN samples s "
        "ON s.id=i.sample_id WHERE i.collection_id=c.id AND s.deleted=0) FROM collections c WHERE c.deleted=0 "
        "ORDER BY c.name");
    if (!statement) return std::unexpected(statement.error());
    std::vector<CollectionRecord> collections;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        CollectionRecord record{
            row.column_int64(0), uuids::uuid::from_string(row.column_text(1)).value_or(uuids::uuid()),
            row.column_text(2),  row.column_text(3) == "smart",
            std::nullopt,        row.column_int64(5)};
        if (auto query = row.column_optional_text(4)) {
            auto document = nlohmann::json::parse(*query, nullptr, false);
            if (document.is_object() && document.contains("text")) record.query = document["text"].get<std::string>();
        }
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
    auto statement = connection.prepare(
        "SELECT s.uuid, s.display_name, s.content_revision, s.edit_revision, s.favorite, s.rating, s.notes, "
        "s.category_id, s.bpm_override_state, s.bpm_override_milli, s.key_override_state, s.key_override_class, "
        "s.key_override_mode, e.format, e.codec, e.sample_rate, e.channels, e.bit_depth, e.source_frames, "
        "e.duration_us, e.duration_exact, e.byte_size, e.bpm_milli, e.bpm_source, e.bpm_confidence, e.key_class, "
        "e.key_mode, e.key_source, e.key_confidence, s.created_at, s.updated_at, s.deleted "
        "FROM samples s LEFT JOIN effective_metadata e ON e.sample_id=s.id WHERE s.id=?1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, sample_id);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::NotFound, "sample not found");

    SampleDetail detail;
    const auto& row = *statement;
    detail.id = sample_id;
    detail.uuid = uuids::uuid::from_string(row.column_text(0)).value_or(uuids::uuid());
    detail.display_name = row.column_text(1);
    detail.content_revision = row.column_int64(2);
    detail.edit_revision = row.column_int64(3);
    detail.favorite = row.column_int64(4) != 0;
    detail.rating = static_cast<int>(row.column_int64(5));
    detail.notes = row.column_text(6);
    detail.category_id = row.column_optional_int64(7);
    detail.bpm_override = parse_override_state(row.column_text(8)).value_or(OverrideState::Automatic);
    detail.bpm_override_milli = row.column_optional_int64(9);
    detail.key_override = parse_override_state(row.column_text(10)).value_or(OverrideState::Automatic);
    detail.key_override_value = key_from_columns(row.column_optional_int64(11), row.column_optional_text(12));
    detail.format = row.column_optional_text(13).value_or("");
    detail.codec = row.column_optional_text(14).value_or("");
    detail.sample_rate = row.column_optional_int64(15);
    detail.channels = row.column_optional_int64(16);
    detail.bit_depth = row.column_optional_int64(17);
    detail.frames = row.column_optional_int64(18);
    detail.duration_us = row.column_optional_int64(19);
    detail.duration_exact = row.column_int64(20) != 0;
    detail.byte_size = row.column_optional_int64(21);
    detail.bpm_milli = row.column_optional_int64(22);
    detail.bpm_source = row.column_optional_text(23).value_or("");
    detail.bpm_confidence = row.column_optional_double(24);
    detail.key = key_from_columns(row.column_optional_int64(25), row.column_optional_text(26));
    detail.key_source = row.column_optional_text(27).value_or("");
    detail.key_confidence = row.column_optional_double(28);
    detail.created_at = row.column_int64(29);
    detail.updated_at = row.column_int64(30);
    detail.deleted = row.column_int64(31) != 0;
    statement->reset();

    if (detail.category_id) {
        auto path = detail::taxonomy_path(connection, detail::Taxonomy::Categories, *detail.category_id);
        if (!path) return std::unexpected(path.error());
        detail.category_path = *path;
    }

    auto tags = connection.prepare(
        "SELECT t.id, t.uuid, t.name FROM sample_tags st JOIN tags t ON t.id=st.tag_id "
        "WHERE st.sample_id=?1 ORDER BY t.name_key");
    if (!tags) return std::unexpected(tags.error());
    tags->bind(1, sample_id);
    auto walked = detail::for_each_row(*tags, [&](sqlite::PreparedStatement& tag) -> Result<void> {
        TaxonomyNode node{tag.column_int64(0),
                          uuids::uuid::from_string(tag.column_text(1)).value_or(uuids::uuid()),
                          std::nullopt,
                          tag.column_text(2),
                          {},
                          0,
                          std::nullopt};
        auto path = detail::taxonomy_path(connection, detail::Taxonomy::Tags, node.id);
        if (!path) return std::unexpected(path.error());
        node.path = *path;
        detail.tags.push_back(std::move(node));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());

    auto collections = connection.prepare(
        "SELECT c.id, c.uuid, c.name FROM collection_items i JOIN collections c ON c.id=i.collection_id "
        "WHERE i.sample_id=?1 AND c.deleted=0 ORDER BY c.name");
    if (!collections) return std::unexpected(collections.error());
    collections->bind(1, sample_id);
    walked = detail::for_each_row(*collections, [&](sqlite::PreparedStatement& collection) -> Result<void> {
        detail.collections.push_back(CollectionRecord{
            collection.column_int64(0), uuids::uuid::from_string(collection.column_text(1)).value_or(uuids::uuid()),
            collection.column_text(2), false, std::nullopt, 0});
        return {};
    });
    if (!walked) return std::unexpected(walked.error());

    auto locations = connection.prepare(
        "SELECT l.id, l.root_id, r.name, r.uuid, r.mode, l.relative_path, l.state, l.byte_size, l.mtime_ns, "
        "lower(hex(l.byte_hash)) FROM locations l JOIN roots r ON r.id=l.root_id WHERE l.sample_id=?1 AND "
        "l.state<>'retired' "
        "ORDER BY l.id");
    if (!locations) return std::unexpected(locations.error());
    locations->bind(1, sample_id);
    walked = detail::for_each_row(*locations, [&](sqlite::PreparedStatement& location) -> Result<void> {
        LocationRecord record;
        record.id = location.column_int64(0);
        record.root_id = location.column_int64(1);
        record.root_name = location.column_text(2);
        record.root_uuid = uuids::uuid::from_string(location.column_text(3)).value_or(uuids::uuid());
        record.root_mode = parse_root_mode(location.column_text(4)).value_or(RootMode::Referenced);
        record.relative_path = location.column_text(5);
        record.state = parse_location_state(location.column_text(6)).value_or(LocationState::Unknown);
        record.byte_size = location.column_optional_int64(7);
        record.mtime_ns = location.column_optional_int64(8);
        if (!location.column_is_null(9)) record.byte_hash_hex = location.column_text(9);
        detail.locations.push_back(std::move(record));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    if (!detail.locations.empty()) {
        const auto& path = detail.locations.front().relative_path;
        const auto slash = path.rfind('/');
        detail.original_name = slash == std::string::npos ? path : path.substr(slash + 1);
    }

    auto observations = connection.prepare(
        "SELECT field, value_json, source, algorithm_version, confidence, content_revision, valid, created_at "
        "FROM metadata_observations WHERE sample_id=?1 ORDER BY id DESC LIMIT 100");
    if (!observations) return std::unexpected(observations.error());
    observations->bind(1, sample_id);
    walked = detail::for_each_row(*observations, [&](sqlite::PreparedStatement& observation) -> Result<void> {
        detail.observations.push_back(ObservationRecord{
            observation.column_text(0), observation.column_text(1), observation.column_text(2),
            observation.column_optional_text(3), observation.column_optional_double(4), observation.column_int64(5),
            observation.column_int64(6) != 0, observation.column_int64(7)});
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return detail;
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
    statement->bind(1, static_cast<std::int64_t>(limit));
    std::vector<ReviewItem> items;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        items.push_back(ReviewItem{row.column_int64(0),
                                   uuids::uuid::from_string(row.column_text(1)).value_or(uuids::uuid()),
                                   row.column_text(2), row.column_optional_int64(3), row.column_text(4),
                                   row.column_text(5), row.column_int64(6)});
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
