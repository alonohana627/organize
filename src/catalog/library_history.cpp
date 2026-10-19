// User edits, structural taxonomy edits, and durable undo/redo.
#include <algorithm>

#include <picosha2.h>
#include <format>

#include "mso/catalog/catalog_internal.hpp"
#include "mso/catalog/library.hpp"
#include "mso/catalog/patch_operations.hpp"

namespace mso::catalog {
using detail::PatchDirection;
namespace patch = detail::patch;

namespace {

/// Canonical digest of a command's content (everything except its operation id), used to detect
/// the same operation id being reused for a different request.
std::string command_digest(const EditCommand& command) {
    nlohmann::json document = {{"label", command.label}, {"samples", command.sample_ids}};
    if (command.rating) document["rating"] = *command.rating;
    if (command.favorite) document["favorite"] = *command.favorite;
    if (command.notes) document["notes"] = *command.notes;
    if (command.display_name) document["display_name"] = *command.display_name;
    document["tags_add"] = command.tags_add;
    document["tags_remove"] = command.tags_remove;
    if (command.category)
        document["category"] = command.category->has_value() ? nlohmann::json(**command.category) : nlohmann::json();
    if (command.bpm)
        document["bpm"] = {
            {"state", to_string(command.bpm->state)},
            {"milli", command.bpm->milli_bpm ? nlohmann::json(*command.bpm->milli_bpm) : nlohmann::json()}};
    if (command.key)
        document["key"] = {
            {"state", to_string(command.key->state)},
            {"key", command.key->key ? nlohmann::json(format_key(*command.key->key)) : nlohmann::json()}};
    if (command.add_to_collection) document["add_to_collection"] = *command.add_to_collection;
    if (command.remove_from_collection) document["remove_from_collection"] = *command.remove_from_collection;
    if (command.removed) document["removed"] = *command.removed;
    return picosha2::hash256_hex_string(document.dump());
}

std::string command_digest(const TaxonomyCommand& command) {
    nlohmann::json document = {
        {"kind", static_cast<int>(command.kind)},
        {"target", command.target_id},
        {"other", command.other_id ? nlohmann::json(*command.other_id) : nlohmann::json()},
        {"parent", command.new_parent_id ? nlohmann::json(*command.new_parent_id) : nlohmann::json()},
        {"name", command.new_name},
        {"query", command.smart_query ? nlohmann::json(*command.smart_query) : nlohmann::json()}};
    return picosha2::hash256_hex_string(document.dump());
}

nlohmann::json receipt_json(const MutationReceipt& receipt) {
    return nlohmann::json{{"operation", uuids::to_string(receipt.operation)},
                          {"revision", receipt.committed_revision},
                          {"action", receipt.history_action ? nlohmann::json(uuids::to_string(*receipt.history_action))
                                                            : nlohmann::json()},
                          {"applied", receipt.applied},
                          {"skipped", receipt.skipped},
                          {"conflicted", receipt.conflicted}};
}

MutationReceipt receipt_from_json(const nlohmann::json& document) {
    MutationReceipt receipt;
    receipt.operation = uuids::uuid::from_string(document.value("operation", std::string())).value_or(uuids::uuid());
    receipt.committed_revision = document.value("revision", std::int64_t{0});
    if (document.contains("action") && document["action"].is_string())
        receipt.history_action = uuids::uuid::from_string(document["action"].get<std::string>());
    receipt.applied = document.value("applied", std::int64_t{0});
    receipt.skipped = document.value("skipped", std::int64_t{0});
    receipt.conflicted = document.value("conflicted", std::int64_t{0});
    receipt.replayed = true;
    return receipt;
}

/// Looks up a stored receipt. Same digest → the stored receipt; different digest → mismatch.
Result<std::optional<MutationReceipt>> find_receipt(sqlite::Connection& connection, const uuids::uuid& operation,
                                                    const std::string& digest) {
    auto statement =
        connection.prepare("SELECT command_digest, receipt_json FROM command_receipts WHERE operation_uuid=?1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, uuids::to_string(operation));
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return std::optional<MutationReceipt>{};
    if (statement->column_text(0) != digest)
        return fail(ErrorCode::IdempotencyMismatch, "this operation id was already used for a different request");
    auto document = nlohmann::json::parse(statement->column_text(1), nullptr, false);
    statement->reset();
    return std::optional<MutationReceipt>(receipt_from_json(document));
}

Result<void> store_receipt(sqlite::Connection& connection, const MutationReceipt& receipt, const std::string& digest,
                           std::string_view state, std::int64_t action_id) {
    auto statement = connection.prepare(
        "INSERT INTO command_receipts(operation_uuid,command_digest,state,action_id,receipt_json,updated_at) "
        "VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(operation_uuid) DO UPDATE SET state=excluded.state, "
        "receipt_json=excluded.receipt_json, updated_at=excluded.updated_at");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, uuids::to_string(receipt.operation)).bind(2, digest).bind(3, state).bind(4, action_id);
    statement->bind(5, receipt_json(receipt).dump()).bind(6, now_unix_ms());
    return statement->run();
}

/// A history action under construction: created with its first chunk, becomes the undo cursor
/// immediately so that a crash mid-way leaves an undoable partial action.
class ActionRecorder {
public:
    explicit ActionRecorder(sqlite::Connection& connection) : connection_(connection) {}

    std::int64_t action_id() const { return action_id_; }
    const uuids::uuid& action_uuid() const { return action_uuid_; }

    /// Creates the action row (state 'applying'), supersedes the redo tail and moves the cursor.
    Result<void> start(std::string_view label) {
        if (auto superseded =
                connection_.execute_script("UPDATE history_actions SET state='superseded' WHERE state='undone'");
            !superseded)
            return superseded;
        action_uuid_ = new_uuid();
        auto insert = connection_.prepare(
            "INSERT INTO history_actions(uuid,label,command_version,state,created_at) VALUES(?1,?2,?3,'applying',?4)");
        if (!insert) return std::unexpected(insert.error());
        insert->bind(1, uuids::to_string(action_uuid_))
            .bind(2, label)
            .bind(3, detail::kHistoryCommandVersion)
            .bind(4, now_unix_ms());
        if (auto inserted = insert->run(); !inserted) return inserted;
        action_id_ = connection_.last_insert_rowid();
        auto cursor = connection_.prepare("UPDATE history_state SET cursor_action_id=?1, active_action_id=?1");
        if (!cursor) return std::unexpected(cursor.error());
        cursor->bind(1, action_id_);
        return cursor->run();
    }

    /// Stores one chunk of operations (forward + inverse) at the given revision.
    Result<void> add_chunk(const nlohmann::json& operations, std::int64_t revision) {
        nlohmann::json inverse = nlohmann::json::array();
        for (auto it = operations.rbegin(); it != operations.rend(); ++it) {
            nlohmann::json reversed = *it;
            std::swap(reversed["before"], reversed["after"]);
            inverse.push_back(std::move(reversed));
        }
        auto insert = connection_.prepare(
            "INSERT INTO history_chunks(action_id,chunk_index,forward_json,inverse_json,state,catalog_revision) "
            "VALUES(?1,?2,?3,?4,'applied',?5)");
        if (!insert) return std::unexpected(insert.error());
        insert->bind(1, action_id_).bind(2, chunk_count_++).bind(3, operations.dump()).bind(4, inverse.dump());
        insert->bind(5, revision);
        return insert->run();
    }

    Result<void> finish() {
        auto update = connection_.prepare("UPDATE history_actions SET state='applied', completed_at=?2 WHERE id=?1");
        if (!update) return std::unexpected(update.error());
        update->bind(1, action_id_).bind(2, now_unix_ms());
        if (auto updated = update->run(); !updated) return updated;
        return connection_.execute_script("UPDATE history_state SET active_action_id=NULL");
    }

    void attach(std::int64_t action_id, const uuids::uuid& uuid, int chunk_count) {
        action_id_ = action_id;
        action_uuid_ = uuid;
        chunk_count_ = chunk_count;
    }

private:
    sqlite::Connection& connection_;
    std::int64_t action_id_ = 0;
    uuids::uuid action_uuid_;
    int chunk_count_ = 0;
};

/// Translates an EditCommand for one chunk of samples into recorded patch operations,
/// applying each one immediately (forward) inside the caller's transaction.
class EditPlanner {
public:
    EditPlanner(sqlite::Connection& connection, const EditCommand& command)
        : connection_(connection), command_(command) {}

    /// Resolves tag/category paths and validates collections once per command.
    Result<void> prepare() {
        for (const auto& path : command_.tags_add) {
            auto id = detail::resolve_taxonomy_path(connection_, detail::Taxonomy::Tags, path, /*create=*/true);
            if (!id) return std::unexpected(id.error());
            tags_to_add_.push_back(*id);
        }
        for (const auto& path : command_.tags_remove) {
            auto id = detail::resolve_taxonomy_path(connection_, detail::Taxonomy::Tags, path, /*create=*/false);
            if (id)
                tags_to_remove_.push_back(*id);
            else if (id.error().code != ErrorCode::NotFound)
                return std::unexpected(id.error());
        }
        if (command_.category && command_.category->has_value()) {
            auto id =
                detail::resolve_taxonomy_path(connection_, detail::Taxonomy::Categories, **command_.category, true);
            if (!id) return std::unexpected(id.error());
            category_id_ = *id;
        }
        for (const auto collection : {command_.add_to_collection, command_.remove_from_collection}) {
            if (!collection) continue;
            auto kind = connection_.prepare("SELECT kind FROM collections WHERE id=?1 AND deleted=0");
            if (!kind) return std::unexpected(kind.error());
            kind->bind(1, *collection);
            auto has_row = kind->next_row();
            if (!has_row) return std::unexpected(has_row.error());
            if (!*has_row) return fail(ErrorCode::NotFound, "collection not found");
            if (kind->column_text(0) != "manual")
                return fail(ErrorCode::InvalidArgument, "smart collections are defined by their query");
        }
        return {};
    }

    /// Applies the command to each sample of the chunk, recording operations.
    Result<void> apply(std::span<const std::int64_t> sample_ids) {
        for (const auto sample_id : sample_ids) {
            auto deleted = connection_.prepare("SELECT deleted FROM samples WHERE id=?1");
            if (!deleted) return std::unexpected(deleted.error());
            deleted->bind(1, sample_id);
            auto has_row = deleted->next_row();
            if (!has_row) return std::unexpected(has_row.error());
            const bool exists = *has_row;
            const bool is_deleted = exists && deleted->column_int64(0) != 0;
            deleted->reset();
            const bool restoring = command_.removed && !*command_.removed;
            if (!exists || (is_deleted && !restoring)) {
                ++skipped_;
                continue;
            }
            if (auto applied = apply_to_sample(sample_id); !applied) return applied;
            ++applied_;
        }
        return {};
    }

    nlohmann::json take_operations() { return std::exchange(operations_, nlohmann::json::array()); }
    std::set<std::int64_t>& affected() { return affected_; }
    std::int64_t applied_count() const { return applied_; }
    std::int64_t skipped_count() const { return skipped_; }

private:
    /// Records a field change when the value differs, and applies it.
    Result<void> set_field(std::int64_t sample_id, std::string_view field, const nlohmann::json& value) {
        nlohmann::json operation = patch::field(sample_id, field, nlohmann::json(), value);
        auto current = detail::read_current_value(connection_, operation);
        if (!current) return std::unexpected(current.error());
        if (*current == value) return {};
        operation["before"] = *current;
        return record(std::move(operation));
    }

    Result<void> record(nlohmann::json operation) {
        if (auto applied = detail::apply_operation(connection_, operation, PatchDirection::Forward, affected_);
            !applied)
            return applied;
        operations_.push_back(std::move(operation));
        return {};
    }

    Result<void> apply_to_sample(std::int64_t id) {
        const auto& c = command_;
        if (c.removed && !*c.removed)
            if (auto done = set_field(id, "deleted", false); !done) return done;
        if (c.rating)
            if (auto done = set_field(id, "rating", *c.rating); !done) return done;
        if (c.favorite)
            if (auto done = set_field(id, "favorite", *c.favorite); !done) return done;
        if (c.notes)
            if (auto done = set_field(id, "notes", *c.notes); !done) return done;
        if (c.display_name)
            if (auto done = set_field(id, "display_name", trim(*c.display_name)); !done) return done;
        if (c.category)
            if (auto done =
                    set_field(id, "category_id", category_id_ ? nlohmann::json(*category_id_) : nlohmann::json());
                !done)
                return done;
        if (c.bpm) {
            nlohmann::json value = {{"state", to_string(c.bpm->state)},
                                    {"milli", c.bpm->milli_bpm ? nlohmann::json(*c.bpm->milli_bpm) : nlohmann::json()}};
            if (auto done = set_field(id, "bpm_override", value); !done) return done;
        }
        if (c.key) {
            nlohmann::json value = {
                {"state", to_string(c.key->state)},
                {"pitch_class", c.key->key ? nlohmann::json(c.key->key->pitch_class) : nlohmann::json()},
                {"mode", c.key->key ? nlohmann::json(std::string(to_string(c.key->key->mode))) : nlohmann::json()}};
            if (auto done = set_field(id, "key_override", value); !done) return done;
        }
        for (const auto tag : tags_to_add_)
            if (auto done = link_tag(id, tag, true); !done) return done;
        for (const auto tag : tags_to_remove_)
            if (auto done = link_tag(id, tag, false); !done) return done;
        if (c.add_to_collection)
            if (auto done = link_collection(id, *c.add_to_collection, true); !done) return done;
        if (c.remove_from_collection)
            if (auto done = link_collection(id, *c.remove_from_collection, false); !done) return done;
        if (c.removed && *c.removed)
            if (auto done = set_field(id, "deleted", true); !done) return done;
        return {};
    }

    /// Records only real membership changes, so undo never removes a pre-existing link.
    Result<void> link_tag(std::int64_t sample_id, std::int64_t tag_id, bool linked) {
        nlohmann::json operation = patch::tag_link(sample_id, tag_id, !linked, linked);
        auto current = detail::read_current_value(connection_, operation);
        if (!current) return std::unexpected(current.error());
        if (current->get<bool>() == linked) return {};
        return record(std::move(operation));
    }

    Result<void> link_collection(std::int64_t sample_id, std::int64_t collection_id, bool linked) {
        nlohmann::json operation = patch::collection_link(collection_id, sample_id, nlohmann::json(), nlohmann::json());
        auto current = detail::read_current_value(connection_, operation);
        if (!current) return std::unexpected(current.error());
        if (current->is_null() != linked) return {};  // already in the requested state
        if (linked) {
            auto next =
                connection_.prepare("SELECT COALESCE(max(ordinal),0)+1 FROM collection_items WHERE collection_id=?1");
            if (!next) return std::unexpected(next.error());
            next->bind(1, collection_id);
            auto has_row = next->next_row();
            if (!has_row) return std::unexpected(has_row.error());
            operation["after"] = next->column_int64(0);
            next->reset();
        } else {
            operation["before"] = *current;
        }
        return record(std::move(operation));
    }

    sqlite::Connection& connection_;
    const EditCommand& command_;
    std::vector<std::int64_t> tags_to_add_;
    std::vector<std::int64_t> tags_to_remove_;
    std::optional<std::int64_t> category_id_;
    nlohmann::json operations_ = nlohmann::json::array();
    std::set<std::int64_t> affected_;
    std::int64_t applied_ = 0;
    std::int64_t skipped_ = 0;
};

/// Checks whether `candidate` lies in the subtree rooted at `ancestor` (inclusive).
Result<bool> is_in_subtree(sqlite::Connection& connection, std::string_view table, std::int64_t ancestor,
                           std::int64_t candidate) {
    auto statement = connection.prepare(std::format(
        "WITH RECURSIVE chain(id,parent_id,depth) AS (SELECT id,parent_id,0 FROM {0} WHERE id=?1 "
        "UNION ALL SELECT t.id,t.parent_id,c.depth+1 FROM {0} t JOIN chain c ON t.id=c.parent_id WHERE c.depth<32) "
        "SELECT 1 FROM chain WHERE id=?2",
        table));
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, candidate).bind(2, ancestor);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    statement->reset();
    return *has_row;
}

/// Rejects a name that a live sibling already uses (normalized comparison).
Result<void> ensure_unique_sibling(sqlite::Connection& connection, std::string_view table, std::int64_t self,
                                   std::optional<std::int64_t> parent, std::string_view name) {
    auto statement = connection.prepare(std::format(
        "SELECT 1 FROM {} WHERE COALESCE(parent_id,0)=COALESCE(?1,0) AND name_key=?2 AND deleted=0 AND id<>?3", table));
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, parent).bind(2, fold_key(name)).bind(3, self);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    statement->reset();
    if (*has_row) return fail(ErrorCode::InvalidArgument, std::format("a sibling named '{}' already exists", name));
    return {};
}

/// Plans a structural edit into recorded operations (applied immediately).
class TaxonomyPlanner {
public:
    TaxonomyPlanner(sqlite::Connection& connection, const TaxonomyCommand& command)
        : connection_(connection), command_(command) {}

    Result<std::string> plan() {
        using Kind = TaxonomyCommand::Kind;
        switch (command_.kind) {
            case Kind::RenameTag:
                return rename("tags", "Rename tag");
            case Kind::RenameCategory:
                return rename("categories", "Rename category");
            case Kind::MoveTag:
                return move_tag();
            case Kind::MergeTag:
                return merge_tag();
            case Kind::CreateCollection:
                return create_collection();
            case Kind::RenameCollection:
                return rename_collection();
            case Kind::DeleteCollection:
                return delete_collection();
        }
        return fail(ErrorCode::InvalidArgument, "unknown taxonomy command");
    }

    nlohmann::json take_operations() { return std::exchange(operations_, nlohmann::json::array()); }
    std::set<std::int64_t>& affected() { return affected_; }

private:
    Result<void> record(nlohmann::json operation) {
        if (auto applied = detail::apply_operation(connection_, operation, PatchDirection::Forward, affected_);
            !applied)
            return applied;
        operations_.push_back(std::move(operation));
        return {};
    }

    Result<nlohmann::json> current(const nlohmann::json& operation) {
        return detail::read_current_value(connection_, operation);
    }

    Result<std::string> rename(std::string_view table, std::string_view verb) {
        const std::string name = trim(command_.new_name);
        if (auto segments = detail::split_taxonomy_path(name); !segments || segments->size() != 1)
            return fail(ErrorCode::InvalidArgument, "a name must be one non-empty segment without '/'");
        const bool is_tag = table == "tags";
        nlohmann::json probe =
            is_tag ? patch::tag_row(command_.target_id, "parent_id", nlohmann::json(), nlohmann::json())
                   : patch::category_row(command_.target_id, "parent_id", nlohmann::json(), nlohmann::json());
        auto parent = current(probe);
        if (!parent) return std::unexpected(parent.error());
        const auto parent_id = parent->is_null() ? std::optional<std::int64_t>() : parent->get<std::int64_t>();
        if (auto unique = ensure_unique_sibling(connection_, table, command_.target_id, parent_id, name); !unique)
            return std::unexpected(unique.error());
        nlohmann::json operation = is_tag ? patch::tag_row(command_.target_id, "name", nlohmann::json(), name)
                                          : patch::category_row(command_.target_id, "name", nlohmann::json(), name);
        auto before = current(operation);
        if (!before) return std::unexpected(before.error());
        operation["before"] = *before;
        if (auto done = record(std::move(operation)); !done) return std::unexpected(done.error());
        return std::format("{} '{}' to '{}'", verb, before->get<std::string>(), name);
    }

    Result<std::string> move_tag() {
        if (command_.new_parent_id) {
            auto cyclic = is_in_subtree(connection_, "tags", command_.target_id, *command_.new_parent_id);
            if (!cyclic) return std::unexpected(cyclic.error());
            if (*cyclic) return fail(ErrorCode::InvalidArgument, "a tag cannot move under itself or its descendants");
            auto depth = detail::taxonomy_path(connection_, detail::Taxonomy::Tags, *command_.new_parent_id);
            if (!depth) return std::unexpected(depth.error());
            if (std::ranges::count(*depth, '/') + 2 > 16)
                return fail(ErrorCode::InvalidArgument, "hierarchy deeper than 16 levels");
        }
        nlohmann::json name_probe = patch::tag_row(command_.target_id, "name", nlohmann::json(), nlohmann::json());
        auto name = current(name_probe);
        if (!name) return std::unexpected(name.error());
        if (auto unique = ensure_unique_sibling(connection_, "tags", command_.target_id, command_.new_parent_id,
                                                name->get<std::string>());
            !unique)
            return std::unexpected(unique.error());
        nlohmann::json operation =
            patch::tag_row(command_.target_id, "parent_id", nlohmann::json(),
                           command_.new_parent_id ? nlohmann::json(*command_.new_parent_id) : nlohmann::json());
        auto before = current(operation);
        if (!before) return std::unexpected(before.error());
        operation["before"] = *before;
        if (auto done = record(std::move(operation)); !done) return std::unexpected(done.error());
        return std::format("Move tag '{}'", name->get<std::string>());
    }

    /// Merge source into canonical: redirect alias, move memberships (remembering pre-existing
    /// target links so undo restores exactly), tombstone the source.
    Result<std::string> merge_tag() {
        if (!command_.other_id) return fail(ErrorCode::InvalidArgument, "merge needs a target tag");
        const auto source = command_.target_id;
        auto target = detail::canonical_tag(connection_, *command_.other_id);
        if (!target) return std::unexpected(target.error());
        if (source == *target) return fail(ErrorCode::InvalidArgument, "a tag cannot be merged into itself");
        auto target_under_source = is_in_subtree(connection_, "tags", source, *target);
        auto source_under_target = is_in_subtree(connection_, "tags", *target, source);
        if (!target_under_source) return std::unexpected(target_under_source.error());
        if (!source_under_target) return std::unexpected(source_under_target.error());
        if (*target_under_source || *source_under_target)
            return fail(ErrorCode::InvalidArgument, "cannot merge a tag with its own ancestor or descendant");
        auto children = connection_.prepare("SELECT 1 FROM tags WHERE parent_id=?1 AND deleted=0 LIMIT 1");
        if (!children) return std::unexpected(children.error());
        children->bind(1, source);
        auto has_children = children->next_row();
        if (!has_children) return std::unexpected(has_children.error());
        children->reset();
        if (*has_children) return fail(ErrorCode::InvalidArgument, "move or merge the tag's children first");

        if (auto done = record(patch::tag_row(source, "alias_of", nlohmann::json(), *target)); !done)
            return std::unexpected(done.error());
        auto members = connection_.prepare("SELECT sample_id FROM sample_tags WHERE tag_id=?1 ORDER BY sample_id");
        if (!members) return std::unexpected(members.error());
        members->bind(1, source);
        std::vector<std::int64_t> samples;
        auto walked = detail::for_each_row(*members, [&](sqlite::PreparedStatement& row) -> Result<void> {
            samples.push_back(row.column_int64(0));
            return {};
        });
        if (!walked) return std::unexpected(walked.error());
        for (const auto sample : samples) {
            if (auto done = record(patch::tag_link(sample, source, true, false)); !done)
                return std::unexpected(done.error());
            nlohmann::json link = patch::tag_link(sample, *target, false, true);
            auto linked = current(link);
            if (!linked) return std::unexpected(linked.error());
            if (!linked->get<bool>())
                if (auto done = record(std::move(link)); !done) return std::unexpected(done.error());
        }
        if (auto done = record(patch::tag_row(source, "deleted", false, true)); !done)
            return std::unexpected(done.error());
        auto source_path = detail::taxonomy_path(connection_, detail::Taxonomy::Tags, source);
        auto target_path = detail::taxonomy_path(connection_, detail::Taxonomy::Tags, *target);
        return std::format("Merge tag '{}' into '{}'", source_path.value_or("?"), target_path.value_or("?"));
    }

    Result<std::string> create_collection() {
        const std::string name = trim(command_.new_name);
        if (name.empty() || count_code_points(name) > 128)
            return fail(ErrorCode::InvalidArgument, "collection name must be 1–128 characters");
        auto insert = connection_.prepare(
            "INSERT INTO collections(uuid,name,kind,query_json,query_version,deleted) VALUES(?1,?2,?3,?4,?5,1)");
        if (!insert) return std::unexpected(insert.error());
        insert->bind(1, uuids::to_string(new_uuid())).bind(2, name).bind(3, command_.smart_query ? "smart" : "manual");
        if (command_.smart_query)
            insert->bind(4, nlohmann::json{{"text", *command_.smart_query}}.dump()).bind(5, 1);
        else
            insert->bind_null(4).bind_null(5);
        if (auto inserted = insert->run(); !inserted) return std::unexpected(inserted.error());
        created_id_ = connection_.last_insert_rowid();
        if (auto done = record(patch::collection_row(created_id_, "deleted", true, false)); !done)
            return std::unexpected(done.error());
        return std::format("Create collection '{}'", name);
    }

    Result<std::string> rename_collection() {
        const std::string name = trim(command_.new_name);
        if (name.empty() || count_code_points(name) > 128)
            return fail(ErrorCode::InvalidArgument, "collection name must be 1–128 characters");
        nlohmann::json operation = patch::collection_row(command_.target_id, "name", nlohmann::json(), name);
        auto before = current(operation);
        if (!before) return std::unexpected(before.error());
        operation["before"] = *before;
        if (auto done = record(std::move(operation)); !done) return std::unexpected(done.error());
        return std::format("Rename collection '{}' to '{}'", before->get<std::string>(), name);
    }

    Result<std::string> delete_collection() {
        if (auto done = record(patch::collection_row(command_.target_id, "deleted", false, true)); !done)
            return std::unexpected(done.error());
        return std::string("Delete collection");
    }

    sqlite::Connection& connection_;
    const TaxonomyCommand& command_;
    nlohmann::json operations_ = nlohmann::json::array();
    std::set<std::int64_t> affected_;
    std::int64_t created_id_ = 0;
};

/// Identifier of the action the cursor points at, if any.
Result<std::optional<std::int64_t>> cursor_action(sqlite::Connection& connection) {
    auto statement = connection.prepare("SELECT cursor_action_id FROM history_state WHERE singleton=1");
    if (!statement) return std::unexpected(statement.error());
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    auto id = *has_row ? statement->column_optional_int64(0) : std::nullopt;
    statement->reset();
    return id;
}

}  // namespace

// ---------------------------------------------------------------- apply_edit

Result<MutationReceipt> Library::apply_edit(const EditCommand& command) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    if (auto valid = validate(command); !valid) return std::unexpected(valid.error());
    const std::string digest = command_digest(command);
    std::lock_guard lock(writer_mutex_);

    if (auto stored = find_receipt(writer_, command.operation, digest); !stored)
        return std::unexpected(stored.error());
    else if (*stored)
        return **stored;

    MutationReceipt receipt{.operation = command.operation};
    ActionRecorder action(writer_);
    EditPlanner planner(writer_, command);
    const std::string label = describe(command);
    const std::span<const std::int64_t> all_samples(command.sample_ids);
    bool prepared = false;

    // Each chunk is its own short transaction; the first one creates the action and receipt.
    for (std::size_t offset = 0; offset < all_samples.size(); offset += EditCommand::kSamplesPerChunk) {
        const auto chunk =
            all_samples.subspan(offset, std::min(EditCommand::kSamplesPerChunk, all_samples.size() - offset));
        const bool last_chunk = offset + chunk.size() >= all_samples.size();
        sqlite::WriteTransaction transaction(writer_);
        if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
        if (!prepared) {
            if (auto ready = planner.prepare(); !ready) return std::unexpected(ready.error());
            if (auto started = action.start(label); !started) return std::unexpected(started.error());
            receipt.history_action = action.action_uuid();
            prepared = true;
        }
        if (auto applied = planner.apply(chunk); !applied) return std::unexpected(applied.error());
        const nlohmann::json operations = planner.take_operations();
        if (auto projected = detail::refresh_search_projection(writer_, planner.affected()); !projected)
            return std::unexpected(projected.error());
        planner.affected().clear();

        nlohmann::json samples = nlohmann::json::array();
        for (const auto id : chunk)
            samples.push_back(id);
        auto revision = detail::advance_revision(
            writer_,
            nlohmann::json{{"kind", "edit"}, {"action", uuids::to_string(action.action_uuid())}, {"samples", samples}});
        if (!revision) return std::unexpected(revision.error());
        if (!operations.empty())
            if (auto chunked = action.add_chunk(operations, *revision); !chunked)
                return std::unexpected(chunked.error());

        receipt.committed_revision = *revision;
        receipt.applied = planner.applied_count();
        receipt.skipped = planner.skipped_count();
        if (last_chunk)
            if (auto finished = action.finish(); !finished) return std::unexpected(finished.error());
        if (auto stored =
                store_receipt(writer_, receipt, digest, last_chunk ? "completed" : "running", action.action_id());
            !stored)
            return std::unexpected(stored.error());
        if (auto committed = transaction.commit(); !committed) return std::unexpected(committed.error());
    }
    return receipt;
}

Result<MutationReceipt> Library::apply_taxonomy(const TaxonomyCommand& command) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    const std::string digest = command_digest(command);
    std::lock_guard lock(writer_mutex_);
    if (auto stored = find_receipt(writer_, command.operation, digest); !stored)
        return std::unexpected(stored.error());
    else if (*stored)
        return **stored;

    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    ActionRecorder action(writer_);
    if (auto started = action.start("structural edit"); !started) return std::unexpected(started.error());
    TaxonomyPlanner planner(writer_, command);
    auto label = planner.plan();
    if (!label) return std::unexpected(label.error());
    auto relabel = writer_.prepare("UPDATE history_actions SET label=?2 WHERE id=?1");
    if (!relabel) return std::unexpected(relabel.error());
    relabel->bind(1, action.action_id()).bind(2, *label);
    if (auto relabeled = relabel->run(); !relabeled) return std::unexpected(relabeled.error());

    if (auto projected = detail::refresh_search_projection(writer_, planner.affected()); !projected)
        return std::unexpected(projected.error());
    auto revision = detail::advance_revision(
        writer_, nlohmann::json{{"kind", "taxonomy"}, {"action", uuids::to_string(action.action_uuid())}});
    if (!revision) return std::unexpected(revision.error());
    if (auto chunked = action.add_chunk(planner.take_operations(), *revision); !chunked)
        return std::unexpected(chunked.error());
    if (auto finished = action.finish(); !finished) return std::unexpected(finished.error());
    MutationReceipt receipt{.operation = command.operation,
                            .committed_revision = *revision,
                            .history_action = action.action_uuid(),
                            .applied = 1};
    if (auto stored = store_receipt(writer_, receipt, digest, "completed", action.action_id()); !stored)
        return std::unexpected(stored.error());
    if (auto committed = transaction.commit(); !committed) return std::unexpected(committed.error());
    return receipt;
}

// ---------------------------------------------------------------- undo / redo

namespace {

/// Applies all chunks of an action in one direction, inside the caller's transaction.
Result<std::int64_t> replay_action(sqlite::Connection& connection, std::int64_t action_id, PatchDirection direction,
                                   std::set<std::int64_t>& affected) {
    const bool undoing = direction == PatchDirection::Inverse;
    auto chunks = connection.prepare(std::format(
        "SELECT chunk_index, forward_json FROM history_chunks WHERE action_id=?1 AND state=?2 ORDER BY chunk_index {}",
        undoing ? "DESC" : "ASC"));
    if (!chunks) return std::unexpected(chunks.error());
    chunks->bind(1, action_id).bind(2, undoing ? "applied" : "undone");
    std::vector<std::pair<std::int64_t, nlohmann::json>> loaded;
    auto walked = detail::for_each_row(*chunks, [&](sqlite::PreparedStatement& row) -> Result<void> {
        loaded.emplace_back(row.column_int64(0), nlohmann::json::parse(row.column_text(1)));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());

    std::int64_t operations_applied = 0;
    for (const auto& [index, operations] : loaded) {
        if (undoing) {
            for (auto it = operations.rbegin(); it != operations.rend(); ++it) {
                if (auto applied = detail::apply_operation(connection, *it, direction, affected); !applied)
                    return std::unexpected(applied.error());
                ++operations_applied;
            }
        } else {
            for (const auto& operation : operations) {
                if (auto applied = detail::apply_operation(connection, operation, direction, affected); !applied)
                    return std::unexpected(applied.error());
                ++operations_applied;
            }
        }
        auto mark = connection.prepare("UPDATE history_chunks SET state=?3 WHERE action_id=?1 AND chunk_index=?2");
        if (!mark) return std::unexpected(mark.error());
        mark->bind(1, action_id).bind(2, index).bind(3, undoing ? "undone" : "applied");
        if (auto marked = mark->run(); !marked) return std::unexpected(marked.error());
    }
    return operations_applied;
}

/// Records a conflict in its own transaction after the failed attempt was rolled back.
void record_history_conflict(sqlite::Connection& connection, std::int64_t action_id, const Error& error) {
    sqlite::WriteTransaction transaction(connection);
    if (!transaction.begin()) return;
    if (!detail::add_review_item(connection, "history_conflict", std::nullopt,
                                 nlohmann::json{{"action_id", action_id}, {"detail", error.safe_detail}}))
        return;
    (void)transaction.commit();
}

}  // namespace

Result<MutationReceipt> Library::undo() {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    std::lock_guard lock(writer_mutex_);
    std::int64_t action_id = 0;
    MutationReceipt receipt{.operation = new_uuid()};
    {
        sqlite::WriteTransaction transaction(writer_);
        if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
        auto cursor = cursor_action(writer_);
        if (!cursor) return std::unexpected(cursor.error());
        if (!*cursor) return fail(ErrorCode::NotFound, "nothing to undo");
        action_id = **cursor;
        auto info = writer_.prepare("SELECT uuid, state FROM history_actions WHERE id=?1");
        if (!info) return std::unexpected(info.error());
        info->bind(1, action_id);
        if (auto has_row = info->next_row(); !has_row || !*has_row)
            return fail(ErrorCode::NotFound, "history cursor is dangling");
        receipt.history_action = uuids::uuid::from_string(info->column_text(0));
        const std::string state = info->column_text(1);
        info->reset();
        if (state != "applied" && state != "partial") return fail(ErrorCode::NotFound, "nothing to undo");

        std::set<std::int64_t> affected;
        auto applied = replay_action(writer_, action_id, PatchDirection::Inverse, affected);
        if (!applied) {
            const Error error = applied.error();
            if (error.code == ErrorCode::HistoryConflict) {
                writer_.rollback();
                record_history_conflict(writer_, action_id, error);
            }
            return std::unexpected(error);
        }
        if (auto projected = detail::refresh_search_projection(writer_, affected); !projected)
            return std::unexpected(projected.error());
        auto mark = writer_.prepare("UPDATE history_actions SET state='undone' WHERE id=?1");
        if (!mark) return std::unexpected(mark.error());
        mark->bind(1, action_id);
        if (auto marked = mark->run(); !marked) return std::unexpected(marked.error());
        auto move = writer_.prepare(
            "UPDATE history_state SET cursor_action_id=(SELECT max(id) FROM history_actions "
            "WHERE id<?1 AND state IN ('applied','partial'))");
        if (!move) return std::unexpected(move.error());
        move->bind(1, action_id);
        if (auto moved = move->run(); !moved) return std::unexpected(moved.error());
        auto revision = detail::advance_revision(writer_, nlohmann::json{{"kind", "undo"}, {"action_id", action_id}});
        if (!revision) return std::unexpected(revision.error());
        receipt.committed_revision = *revision;
        receipt.applied = *applied;
        if (auto committed = transaction.commit(); !committed) return std::unexpected(committed.error());
    }
    return receipt;
}

Result<MutationReceipt> Library::redo() {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    auto next = writer_.prepare("SELECT id, uuid FROM history_actions WHERE state='undone' ORDER BY id LIMIT 1");
    if (!next) return std::unexpected(next.error());
    auto has_row = next->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::NotFound, "nothing to redo");
    const std::int64_t action_id = next->column_int64(0);
    MutationReceipt receipt{.operation = new_uuid(), .history_action = uuids::uuid::from_string(next->column_text(1))};
    next->reset();

    std::set<std::int64_t> affected;
    auto applied = replay_action(writer_, action_id, PatchDirection::Forward, affected);
    if (!applied) {
        const Error error = applied.error();
        if (error.code == ErrorCode::HistoryConflict) {
            writer_.rollback();
            record_history_conflict(writer_, action_id, error);
        }
        return std::unexpected(error);
    }
    if (auto projected = detail::refresh_search_projection(writer_, affected); !projected)
        return std::unexpected(projected.error());
    auto mark = writer_.prepare("UPDATE history_actions SET state='applied' WHERE id=?1");
    if (!mark) return std::unexpected(mark.error());
    mark->bind(1, action_id);
    if (auto marked = mark->run(); !marked) return std::unexpected(marked.error());
    auto move = writer_.prepare("UPDATE history_state SET cursor_action_id=?1");
    if (!move) return std::unexpected(move.error());
    move->bind(1, action_id);
    if (auto moved = move->run(); !moved) return std::unexpected(moved.error());
    auto revision = detail::advance_revision(writer_, nlohmann::json{{"kind", "redo"}, {"action_id", action_id}});
    if (!revision) return std::unexpected(revision.error());
    receipt.committed_revision = *revision;
    receipt.applied = *applied;
    if (auto committed = transaction.commit(); !committed) return std::unexpected(committed.error());
    return receipt;
}

Result<std::vector<HistoryEntry>> Library::history(std::size_t limit) {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto statement = reader->connection().prepare(
        "SELECT a.id, a.uuid, a.label, a.state, a.created_at, (a.id = h.cursor_action_id) "
        "FROM history_actions a, history_state h WHERE a.state<>'superseded' ORDER BY a.id DESC LIMIT ?1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, static_cast<std::int64_t>(limit));
    std::vector<HistoryEntry> entries;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        entries.push_back(
            HistoryEntry{row.column_int64(0), uuids::uuid::from_string(row.column_text(1)).value_or(uuids::uuid()),
                         row.column_text(2), row.column_text(3), row.column_int64(4), row.column_int64(5) != 0});
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return entries;
}

}  // namespace mso::catalog
