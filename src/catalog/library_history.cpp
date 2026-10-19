// User edits, structural taxonomy edits, and durable undo/redo.
#include <algorithm>
#include <format>

#include <picosha2.h>

#include "mso/catalog/detail/catalog_internal.hpp"
#include "mso/catalog/detail/patch_operations.hpp"
#include "mso/catalog/library.hpp"

namespace mso::catalog {
using detail::PatchDirection;
using detail::PatchEffects;
namespace patch = detail::patch;

namespace {

constexpr std::size_t kMaximumCollectionNameCodePoints = 128;

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
    return picosha2::hash256_hex_string(json_fields::dump_json(document));
}

std::string command_digest(const TaxonomyCommand& command) {
    const nlohmann::json document = {
        {"kind", static_cast<int>(command.kind)},
        {"target", command.target_id},
        {"other", command.other_id ? nlohmann::json(*command.other_id) : nlohmann::json()},
        {"parent", command.new_parent_id ? nlohmann::json(*command.new_parent_id) : nlohmann::json()},
        {"name", command.new_name},
        {"query", command.smart_query ? nlohmann::json(*command.smart_query) : nlohmann::json()}};
    return picosha2::hash256_hex_string(json_fields::dump_json(document));
}

/// Digest of an undo or redo request (they carry no content beyond their direction).
std::string history_step_digest(bool undoing) {
    return picosha2::hash256_hex_string(std::string(undoing ? "{\"kind\":\"undo\"}" : "{\"kind\":\"redo\"}"));
}

nlohmann::json receipt_json(const MutationReceipt& receipt, std::size_t next_offset) {
    return nlohmann::json{{"operation", uuids::to_string(receipt.operation)},
                          {"revision", receipt.committed_revision},
                          {"action", receipt.history_action ? nlohmann::json(uuids::to_string(*receipt.history_action))
                                                            : nlohmann::json()},
                          {"applied", receipt.applied},
                          {"skipped", receipt.skipped},
                          {"conflicted", receipt.conflicted},
                          {"next_offset", next_offset}};
}

/// A receipt read back from command_receipts.
struct StoredReceipt {
    MutationReceipt receipt;
    std::string state;  ///< running, completed, partial, failed
    std::optional<std::int64_t> action_id;
    std::size_t next_offset = 0;  ///< samples of the command already processed (chunked edits)
};

Result<StoredReceipt> stored_receipt_from(const std::string& text, std::string state,
                                          std::optional<std::int64_t> action_id) {
    const auto document = json_fields::parse_json(text);
    const auto operation = json_fields::string_field(document, "operation");
    const auto parsed_operation = operation ? uuids::uuid::from_string(*operation) : std::nullopt;
    if (document.is_discarded() || !parsed_operation) return fail(ErrorCode::DbCorrupt, "stored receipt is malformed");
    StoredReceipt stored;
    stored.state = std::move(state);
    stored.action_id = action_id;
    stored.receipt.operation = *parsed_operation;
    stored.receipt.committed_revision = json_fields::integer_field(document, "revision").value_or(0);
    if (const auto action = json_fields::string_field(document, "action")) {
        const auto action_uuid = uuids::uuid::from_string(*action);
        if (!action_uuid) return fail(ErrorCode::DbCorrupt, "stored receipt names a malformed action");
        stored.receipt.history_action = *action_uuid;
    }
    stored.receipt.applied = json_fields::integer_field(document, "applied").value_or(0);
    stored.receipt.skipped = json_fields::integer_field(document, "skipped").value_or(0);
    stored.receipt.conflicted = json_fields::integer_field(document, "conflicted").value_or(0);
    stored.next_offset = static_cast<std::size_t>(
        std::max<std::int64_t>(0, json_fields::integer_field(document, "next_offset").value_or(0)));
    stored.receipt.replayed = true;
    stored.receipt.partial = stored.state != "completed";
    return stored;
}

/// Looks up a stored receipt. Same digest → the stored receipt; different digest → mismatch.
Result<std::optional<StoredReceipt>> find_receipt(sqlite::Connection& connection, const uuids::uuid& operation,
                                                  const std::string& digest) {
    auto statement = connection.prepare(
        "SELECT command_digest, receipt_json, state, action_id FROM command_receipts WHERE operation_uuid=?1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, uuids::to_string(operation));
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return std::optional<StoredReceipt>{};
    const bool same_request = statement->column_text(0) == digest;
    const std::string text = statement->column_text(1);
    std::string state = statement->column_text(2);
    const auto action_id = statement->column_optional_int64(3);
    statement->reset();
    if (!same_request)
        return fail(ErrorCode::IdempotencyMismatch, "this operation id was already used for a different request");
    auto stored = stored_receipt_from(text, std::move(state), action_id);
    if (!stored) return std::unexpected(stored.error());
    return std::optional<StoredReceipt>(std::move(*stored));
}

Result<void> store_receipt(sqlite::Connection& connection, const MutationReceipt& receipt, const std::string& digest,
                           std::string_view state, std::optional<std::int64_t> action_id, std::size_t next_offset = 0) {
    auto statement = connection.prepare(
        "INSERT INTO command_receipts(operation_uuid,command_digest,state,action_id,receipt_json,updated_at) "
        "VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(operation_uuid) DO UPDATE SET state=excluded.state, "
        "receipt_json=excluded.receipt_json, updated_at=excluded.updated_at");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, uuids::to_string(receipt.operation)).bind(2, digest).bind(3, state).bind(4, action_id);
    statement->bind(5, json_fields::dump_json(receipt_json(receipt, next_offset))).bind(6, now_unix_ms());
    return statement->run();
}

/// Runs one parameterized statement binding ?1 = id.
Result<void> run_with_id(sqlite::Connection& connection, std::string_view sql, std::int64_t id) {
    auto statement = connection.prepare(sql);
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, id);
    return statement->run();
}

/// A history action under construction: created with its first chunk, becomes the undo cursor
/// immediately so that a crash mid-way leaves an undoable partial action.
class ActionRecorder {
public:
    explicit ActionRecorder(sqlite::Connection& connection) : connection_(connection) {}

    /// The action row id (0 until started or attached).
    std::int64_t action_id() const { return action_id_; }
    /// The action's UUID.
    const uuids::uuid& action_uuid() const { return action_uuid_; }
    /// True once the action exists in the database.
    bool exists() const { return action_id_ != 0; }

    /// Creates the action row (state 'applying'), supersedes the redo tail and moves the cursor.
    /// History is kept without automatic pruning (docs/03_DATA_AND_SEARCH.md §9).
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
        return run_with_id(connection_, "UPDATE history_state SET cursor_action_id=?1", action_id_);
    }

    /// Adopts an existing partial action (a retried chunked edit) without touching the database.
    void adopt(std::int64_t action_id, const uuids::uuid& uuid) {
        action_id_ = action_id;
        action_uuid_ = uuid;
    }

    /// Continues the adopted action: it becomes 'applying' again and new chunks continue its numbering.
    Result<void> resume() {
        auto next = connection_.prepare("SELECT COALESCE(max(chunk_index)+1,0) FROM history_chunks WHERE action_id=?1");
        if (!next) return std::unexpected(next.error());
        next->bind(1, action_id_);
        auto has_row = next->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        next_chunk_index_ = *has_row ? next->column_int64(0) : 0;
        next->reset();
        return run_with_id(connection_, "UPDATE history_actions SET state='applying' WHERE id=?1", action_id_);
    }

    /// Stores one chunk of operations at the given revision. The inverse column of the released
    /// schema is unused (undo reverses the forward operations), so it stores an empty array.
    Result<void> add_chunk(const nlohmann::json& operations, std::int64_t revision) {
        auto insert = connection_.prepare(
            "INSERT INTO history_chunks(action_id,chunk_index,forward_json,inverse_json,state,catalog_revision) "
            "VALUES(?1,?2,?3,'[]','applied',?4)");
        if (!insert) return std::unexpected(insert.error());
        insert->bind(1, action_id_).bind(2, next_chunk_index_++).bind(3, json_fields::dump_json(operations));
        insert->bind(4, revision);
        return insert->run();
    }

    /// Marks the action completely applied. (history_state.active_action_id of the released schema
    /// is retired: an action in progress is the one in state 'applying', which recovery marks partial.)
    Result<void> finish() {
        auto update = connection_.prepare("UPDATE history_actions SET state='applied', completed_at=?2 WHERE id=?1");
        if (!update) return std::unexpected(update.error());
        update->bind(1, action_id_).bind(2, now_unix_ms());
        return update->run();
    }

    /// Marks the action partially applied (its committed chunks stay undoable).
    Result<void> mark_partial() {
        return run_with_id(connection_, "UPDATE history_actions SET state='partial' WHERE id=?1", action_id_);
    }

    /// The action row id for a receipt: nullopt while no action was created.
    std::optional<std::int64_t> recorded_action_id() const {
        return exists() ? std::optional<std::int64_t>(action_id_) : std::nullopt;
    }

private:
    sqlite::Connection& connection_;
    std::int64_t action_id_ = 0;
    uuids::uuid action_uuid_;
    std::int64_t next_chunk_index_ = 0;
};

/// Translates an EditCommand for one chunk of samples into recorded patch operations,
/// applying each one immediately (forward) inside the caller's transaction.
class EditPlanner {
public:
    EditPlanner(sqlite::Connection& connection, const EditCommand& command)
        : connection_(connection), command_(command) {}

    /// Starts the counters from an earlier, partially applied run of the same command.
    void continue_counts(std::int64_t applied, std::int64_t skipped) {
        applied_ = applied;
        skipped_ = skipped;
    }

    /// Resolves tag/category paths and validates collections once per command. Tags and categories
    /// created here are recorded as undoable operations of the action.
    Result<void> prepare() {
        std::vector<std::int64_t> created_tags;
        std::vector<std::int64_t> created_categories;
        for (const auto& path : command_.tags_add) {
            auto id = detail::resolve_taxonomy_path(connection_, detail::Taxonomy::Tags, path, /*create=*/true,
                                                    &created_tags);
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
            auto id = detail::resolve_taxonomy_path(connection_, detail::Taxonomy::Categories, **command_.category,
                                                    /*create=*/true, &created_categories);
            if (!id) return std::unexpected(id.error());
            category_id_ = *id;
        }
        // Creation already happened: record it as applied so undo removes the new nodes again.
        for (const auto id : created_tags)
            operations_.push_back(patch::tag_row(id, "deleted", true, false));
        for (const auto id : created_categories)
            operations_.push_back(patch::category_row(id, "deleted", true, false));
        if (!created_tags.empty() || !created_categories.empty()) effects_.structure_changed = true;

        for (const auto collection : {command_.add_to_collection, command_.remove_from_collection}) {
            if (!collection) continue;
            auto kind = connection_.prepare("SELECT kind FROM collections WHERE id=?1 AND deleted=0");
            if (!kind) return std::unexpected(kind.error());
            kind->bind(1, *collection);
            auto has_row = kind->next_row();
            if (!has_row) return std::unexpected(has_row.error());
            if (!*has_row) return fail(ErrorCode::NotFound, "collection not found");
            const bool manual = kind->column_text(0) == "manual";
            kind->reset();
            if (!manual) return fail(ErrorCode::InvalidArgument, "smart collections are defined by their query");
        }
        return {};
    }

    /// Applies the command to each sample of the chunk, recording operations.
    Result<void> apply(std::span<const std::int64_t> sample_ids) {
        auto deleted = connection_.prepare("SELECT deleted FROM samples WHERE id=?1");
        if (!deleted) return std::unexpected(deleted.error());
        for (const auto sample_id : sample_ids) {
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

    /// Operations recorded since the last call.
    nlohmann::json take_operations() { return std::exchange(operations_, nlohmann::json::array()); }
    /// Structural effects of everything applied so far.
    const PatchEffects& effects() const { return effects_; }
    /// Samples the command was applied to so far.
    std::int64_t applied_count() const { return applied_; }
    /// Samples skipped so far (missing or removed).
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
        if (auto applied = detail::apply_operation(connection_, operation, PatchDirection::Forward, effects_); !applied)
            return applied;
        operations_.push_back(std::move(operation));
        return {};
    }

    Result<void> apply_to_sample(std::int64_t sample_id) {
        const auto& command = command_;
        if (command.removed && !*command.removed)
            if (auto done = set_field(sample_id, "deleted", false); !done) return done;
        if (command.rating)
            if (auto done = set_field(sample_id, "rating", *command.rating); !done) return done;
        if (command.favorite)
            if (auto done = set_field(sample_id, "favorite", *command.favorite); !done) return done;
        if (command.notes)
            if (auto done = set_field(sample_id, "notes", *command.notes); !done) return done;
        if (command.display_name)
            if (auto done = set_field(sample_id, "display_name", trim(*command.display_name)); !done) return done;
        if (command.category)
            if (auto done = set_field(sample_id, "category_id",
                                      category_id_ ? nlohmann::json(*category_id_) : nlohmann::json());
                !done)
                return done;
        if (command.bpm) {
            const nlohmann::json value = {
                {"state", to_string(command.bpm->state)},
                {"milli", command.bpm->milli_bpm ? nlohmann::json(*command.bpm->milli_bpm) : nlohmann::json()}};
            if (auto done = set_field(sample_id, "bpm_override", value); !done) return done;
        }
        if (command.key) {
            const auto& key = command.key->key;
            const nlohmann::json value = {
                {"state", to_string(command.key->state)},
                {"pitch_class", key ? nlohmann::json(key->pitch_class) : nlohmann::json()},
                {"mode", key ? nlohmann::json(std::string(to_string(key->mode))) : nlohmann::json()}};
            if (auto done = set_field(sample_id, "key_override", value); !done) return done;
        }
        for (const auto tag : tags_to_add_)
            if (auto done = link_tag(sample_id, tag, true); !done) return done;
        for (const auto tag : tags_to_remove_)
            if (auto done = link_tag(sample_id, tag, false); !done) return done;
        if (command.add_to_collection)
            if (auto done = link_collection(sample_id, *command.add_to_collection, true); !done) return done;
        if (command.remove_from_collection)
            if (auto done = link_collection(sample_id, *command.remove_from_collection, false); !done) return done;
        if (command.removed && *command.removed)
            if (auto done = set_field(sample_id, "deleted", true); !done) return done;
        return {};
    }

    /// Records only real membership changes, so undo never removes a pre-existing link.
    Result<void> link_tag(std::int64_t sample_id, std::int64_t tag_id, bool linked) {
        nlohmann::json operation = patch::tag_link(sample_id, tag_id, !linked, linked);
        auto current = detail::read_current_value(connection_, operation);
        if (!current) return std::unexpected(current.error());
        if (*current == nlohmann::json(linked)) return {};
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
            operation["after"] = *has_row ? next->column_int64(0) : 1;
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
    PatchEffects effects_;
    std::int64_t applied_ = 0;
    std::int64_t skipped_ = 0;
};

/// Checks whether `candidate` lies in the subtree rooted at `ancestor` (inclusive).
Result<bool> is_in_subtree(sqlite::Connection& connection, std::string_view table, std::int64_t ancestor,
                           std::int64_t candidate) {
    auto statement = connection.prepare(std::format(
        "WITH RECURSIVE chain(id,parent_id,depth) AS (SELECT id,parent_id,0 FROM {0} WHERE id=?1 "
        "UNION ALL SELECT node.id,node.parent_id,chain.depth+1 FROM {0} node JOIN chain ON node.id=chain.parent_id "
        "WHERE chain.depth<32) SELECT 1 FROM chain WHERE id=?2",
        table));
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, candidate).bind(2, ancestor);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    statement->reset();
    return *has_row;
}

/// Levels below a node in its live subtree (0 for a leaf).
Result<std::int64_t> subtree_height(sqlite::Connection& connection, std::string_view table, std::int64_t node) {
    auto statement = connection.prepare(std::format(
        "WITH RECURSIVE subtree(id,depth) AS (SELECT ?1,0 UNION ALL SELECT child.id,subtree.depth+1 FROM {0} child "
        "JOIN subtree ON child.parent_id=subtree.id WHERE child.deleted=0 AND subtree.depth<32) "
        "SELECT max(depth) FROM subtree",
        table));
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, node);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    const auto height = *has_row ? statement->column_int64(0) : 0;
    statement->reset();
    return height;
}

/// NOT_FOUND unless the row exists and is not deleted.
Result<void> ensure_live(sqlite::Connection& connection, std::string_view table, std::int64_t id,
                         std::string_view what) {
    auto statement = connection.prepare(std::format("SELECT deleted FROM {} WHERE id=?1", table));
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, id);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    const bool live = *has_row && statement->column_int64(0) == 0;
    statement->reset();
    if (!live) return fail(ErrorCode::NotFound, std::format("{} {} does not exist", what, id));
    return {};
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

/// Samples (sorted) linked to `tag`; with `lacking_tag`, only those not linked to that tag too.
Result<std::vector<std::int64_t>> tag_members(sqlite::Connection& connection, std::int64_t tag,
                                              std::optional<std::int64_t> lacking_tag) {
    auto statement = connection.prepare(
        "SELECT link.sample_id FROM sample_tags link WHERE link.tag_id=?1 AND (?2 IS NULL OR NOT EXISTS("
        "SELECT 1 FROM sample_tags other WHERE other.sample_id=link.sample_id AND other.tag_id=?2)) "
        "ORDER BY link.sample_id");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, tag).bind(2, lacking_tag);
    std::vector<std::int64_t> samples;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        samples.push_back(row.column_int64(0));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return samples;
}

/// Plans a structural edit into recorded operations (applied immediately).
class TaxonomyPlanner {
public:
    TaxonomyPlanner(sqlite::Connection& connection, const TaxonomyCommand& command)
        : connection_(connection), command_(command) {}

    /// Validates and applies the command; returns the history label.
    Result<std::string> plan() {
        using Kind = TaxonomyCommand::Kind;
        switch (command_.kind) {
            case Kind::RenameTag:
                return rename(detail::Taxonomy::Tags, "Rename tag");
            case Kind::RenameCategory:
                return rename(detail::Taxonomy::Categories, "Rename category");
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

    /// Operations recorded by plan().
    nlohmann::json take_operations() { return std::exchange(operations_, nlohmann::json::array()); }
    /// Effects of the applied operations.
    const PatchEffects& effects() const { return effects_; }

private:
    Result<void> record(nlohmann::json operation) {
        if (auto applied = detail::apply_operation(connection_, operation, PatchDirection::Forward, effects_); !applied)
            return applied;
        operations_.push_back(std::move(operation));
        return {};
    }

    Result<nlohmann::json> current(const nlohmann::json& operation) {
        return detail::read_current_value(connection_, operation);
    }

    /// Text of a stored string value (rows written by this module always hold strings).
    static std::string text_of(const nlohmann::json& value) {
        return value.is_string() ? value.get<std::string>() : std::string();
    }

    Result<std::string> rename(detail::Taxonomy taxonomy, std::string_view verb) {
        const bool is_tag = taxonomy == detail::Taxonomy::Tags;
        const auto table = detail::taxonomy_table(taxonomy);
        if (auto live = ensure_live(connection_, table, command_.target_id, is_tag ? "tag" : "category"); !live)
            return std::unexpected(live.error());
        const std::string name = trim(command_.new_name);
        if (auto segments = split_taxonomy_path(name); !segments || segments->size() != 1)
            return fail(ErrorCode::InvalidArgument, "a name must be one non-empty segment without '/'");
        const nlohmann::json probe =
            is_tag ? patch::tag_row(command_.target_id, "parent_id", nlohmann::json(), nlohmann::json())
                   : patch::category_row(command_.target_id, "parent_id", nlohmann::json(), nlohmann::json());
        auto parent = current(probe);
        if (!parent) return std::unexpected(parent.error());
        const auto parent_id = json_fields::integer_value(*parent);
        if (auto unique = ensure_unique_sibling(connection_, table, command_.target_id, parent_id, name); !unique)
            return std::unexpected(unique.error());
        nlohmann::json operation = is_tag ? patch::tag_row(command_.target_id, "name", nlohmann::json(), name)
                                          : patch::category_row(command_.target_id, "name", nlohmann::json(), name);
        auto before = current(operation);
        if (!before) return std::unexpected(before.error());
        operation["before"] = *before;
        if (auto done = record(std::move(operation)); !done) return std::unexpected(done.error());
        return std::format("{} '{}' to '{}'", verb, text_of(*before), name);
    }

    Result<std::string> move_tag() {
        if (auto live = ensure_live(connection_, "tags", command_.target_id, "tag"); !live)
            return std::unexpected(live.error());
        auto height = subtree_height(connection_, "tags", command_.target_id);
        if (!height) return std::unexpected(height.error());
        std::int64_t parent_levels = 0;
        if (command_.new_parent_id) {
            if (auto live = ensure_live(connection_, "tags", *command_.new_parent_id, "parent tag"); !live)
                return std::unexpected(live.error());
            auto cyclic = is_in_subtree(connection_, "tags", command_.target_id, *command_.new_parent_id);
            if (!cyclic) return std::unexpected(cyclic.error());
            if (*cyclic) return fail(ErrorCode::InvalidArgument, "a tag cannot move under itself or its descendants");
            auto parent_path = detail::taxonomy_path(connection_, detail::Taxonomy::Tags, *command_.new_parent_id);
            if (!parent_path) return std::unexpected(parent_path.error());
            parent_levels = std::ranges::count(*parent_path, '/') + 1;
        }
        // The moved tag sits at parent_levels + 1; its deepest descendant `height` levels below.
        if (parent_levels + 1 + *height > static_cast<std::int64_t>(kMaxTaxonomyDepth))
            return fail(ErrorCode::InvalidArgument, "hierarchy deeper than 16 levels");
        const nlohmann::json name_probe =
            patch::tag_row(command_.target_id, "name", nlohmann::json(), nlohmann::json());
        auto name = current(name_probe);
        if (!name) return std::unexpected(name.error());
        if (auto unique =
                ensure_unique_sibling(connection_, "tags", command_.target_id, command_.new_parent_id, text_of(*name));
            !unique)
            return std::unexpected(unique.error());
        nlohmann::json operation =
            patch::tag_row(command_.target_id, "parent_id", nlohmann::json(),
                           command_.new_parent_id ? nlohmann::json(*command_.new_parent_id) : nlohmann::json());
        auto before = current(operation);
        if (!before) return std::unexpected(before.error());
        operation["before"] = *before;
        if (auto done = record(std::move(operation)); !done) return std::unexpected(done.error());
        return std::format("Move tag '{}'", text_of(*name));
    }

    /// Merge source into canonical: redirect alias, move memberships with two set-based operations
    /// (remembering pre-existing target links so undo restores exactly), tombstone the source.
    Result<std::string> merge_tag() {
        if (!command_.other_id) return fail(ErrorCode::InvalidArgument, "merge needs a target tag");
        const auto source = command_.target_id;
        if (auto live = ensure_live(connection_, "tags", source, "tag"); !live) return std::unexpected(live.error());
        auto target = detail::canonical_tag(connection_, *command_.other_id);
        if (!target) return std::unexpected(target.error());
        if (auto live = ensure_live(connection_, "tags", *target, "target tag"); !live)
            return std::unexpected(live.error());
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

        auto source_path = detail::taxonomy_path(connection_, detail::Taxonomy::Tags, source);
        auto target_path = detail::taxonomy_path(connection_, detail::Taxonomy::Tags, *target);
        if (!source_path) return std::unexpected(source_path.error());
        if (!target_path) return std::unexpected(target_path.error());
        auto members = tag_members(connection_, source, std::nullopt);
        auto newly_linked = tag_members(connection_, source, *target);
        if (!members) return std::unexpected(members.error());
        if (!newly_linked) return std::unexpected(newly_linked.error());

        nlohmann::json alias = patch::tag_row(source, "alias_of", nlohmann::json(), *target);
        auto alias_before = current(alias);
        if (!alias_before) return std::unexpected(alias_before.error());
        alias["before"] = *alias_before;
        if (auto done = record(std::move(alias)); !done) return std::unexpected(done.error());
        if (!members->empty())
            if (auto done = record(patch::tag_links(source, *members, true, false)); !done)
                return std::unexpected(done.error());
        if (!newly_linked->empty())
            if (auto done = record(patch::tag_links(*target, *newly_linked, false, true)); !done)
                return std::unexpected(done.error());
        if (auto done = record(patch::tag_row(source, "deleted", false, true)); !done)
            return std::unexpected(done.error());
        return std::format("Merge tag '{}' into '{}'", *source_path, *target_path);
    }

    Result<std::string> create_collection() {
        const std::string name = trim(command_.new_name);
        if (name.empty() || !is_valid_utf8(name) || count_code_points(name) > kMaximumCollectionNameCodePoints)
            return fail(ErrorCode::InvalidArgument, "collection name must be 1–128 characters");
        auto insert = connection_.prepare(
            "INSERT INTO collections(uuid,name,kind,query_json,query_version,deleted) "
            "VALUES(new_uuid(),?1,?2,?3,?4,1)");
        if (!insert) return std::unexpected(insert.error());
        insert->bind(1, name).bind(2, command_.smart_query ? "smart" : "manual");
        if (command_.smart_query)
            insert->bind(3, json_fields::dump_json(nlohmann::json{{"text", *command_.smart_query}})).bind(4, 1);
        else
            insert->bind_null(3).bind_null(4);
        if (auto inserted = insert->run(); !inserted) return std::unexpected(inserted.error());
        const auto created_id = connection_.last_insert_rowid();
        if (auto done = record(patch::collection_row(created_id, "deleted", true, false)); !done)
            return std::unexpected(done.error());
        return std::format("Create collection '{}'", name);
    }

    Result<std::string> rename_collection() {
        if (auto live = ensure_live(connection_, "collections", command_.target_id, "collection"); !live)
            return std::unexpected(live.error());
        const std::string name = trim(command_.new_name);
        if (name.empty() || !is_valid_utf8(name) || count_code_points(name) > kMaximumCollectionNameCodePoints)
            return fail(ErrorCode::InvalidArgument, "collection name must be 1–128 characters");
        nlohmann::json operation = patch::collection_row(command_.target_id, "name", nlohmann::json(), name);
        auto before = current(operation);
        if (!before) return std::unexpected(before.error());
        operation["before"] = *before;
        if (auto done = record(std::move(operation)); !done) return std::unexpected(done.error());
        return std::format("Rename collection '{}' to '{}'", text_of(*before), name);
    }

    Result<std::string> delete_collection() {
        if (auto live = ensure_live(connection_, "collections", command_.target_id, "collection"); !live)
            return std::unexpected(live.error());
        const nlohmann::json name_probe =
            patch::collection_row(command_.target_id, "name", nlohmann::json(), nlohmann::json());
        auto name = current(name_probe);
        if (!name) return std::unexpected(name.error());
        if (auto done = record(patch::collection_row(command_.target_id, "deleted", false, true)); !done)
            return std::unexpected(done.error());
        return std::format("Delete collection '{}'", text_of(*name));
    }

    sqlite::Connection& connection_;
    const TaxonomyCommand& command_;
    nlohmann::json operations_ = nlohmann::json::array();
    PatchEffects effects_;
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

/// Whether a partially applied edit can continue: it is still the newest action at the cursor.
Result<bool> is_resumable(sqlite::Connection& connection, std::int64_t action_id) {
    auto statement = connection.prepare(
        "SELECT action.state IN ('applying','partial') AND history.cursor_action_id=action.id "
        "FROM history_actions action, history_state history WHERE action.id=?1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, action_id);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    const bool resumable = *has_row && statement->column_int64(0) != 0;
    statement->reset();
    return resumable;
}

}  // namespace

// ---------------------------------------------------------------- apply_edit

Result<MutationReceipt> Library::apply_edit(const EditCommand& command) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    if (auto valid = validate(command); !valid) return std::unexpected(valid.error());
    const std::string digest = command_digest(command);
    const std::lock_guard lock(writer_mutex_);

    auto stored = find_receipt(writer_, command.operation, digest);
    if (!stored) return std::unexpected(stored.error());

    MutationReceipt receipt{.operation = command.operation};
    ActionRecorder action(writer_);
    EditPlanner planner(writer_, command);
    std::size_t offset = 0;
    bool resuming = false;
    if (*stored) {
        if ((*stored)->state == "completed") return (*stored)->receipt;
        // An earlier attempt committed some chunks. Continue it only while it is still the newest
        // history entry; otherwise report what was applied as partial.
        const bool has_action = (*stored)->action_id.has_value() && (*stored)->receipt.history_action.has_value();
        auto resumable = has_action ? is_resumable(writer_, *(*stored)->action_id) : Result<bool>(false);
        if (!resumable) return std::unexpected(resumable.error());
        if (!*resumable) return (*stored)->receipt;  // replayed = true, partial = true
        receipt = (*stored)->receipt;
        receipt.replayed = false;
        receipt.partial = false;
        offset = (*stored)->next_offset;
        planner.continue_counts(receipt.applied, receipt.skipped);
        action.adopt(*(*stored)->action_id, *receipt.history_action);
        resuming = true;
    }

    const std::string label = describe(command);
    const std::span<const std::int64_t> all_samples(command.sample_ids);
    bool prepared = false;
    bool committed_any = false;

    // Each chunk is its own short transaction; the first one creates (or resumes) the action.
    auto commit_chunk = [&](std::span<const std::int64_t> chunk, bool last_chunk) -> Result<void> {
        sqlite::WriteTransaction transaction(writer_);
        if (auto began = transaction.begin(); !began) return began;
        if (!prepared) {
            if (auto ready = planner.prepare(); !ready) return ready;
            if (resuming)
                if (auto resumed = action.resume(); !resumed) return resumed;
        }
        if (auto applied = planner.apply(chunk); !applied) return applied;
        const nlohmann::json operations = planner.take_operations();
        // The action is created with the first real change, so an edit that changes nothing leaves
        // history (and the redo tail) untouched.
        if (!operations.empty() && !action.exists()) {
            if (auto started = action.start(label); !started) return started;
            receipt.history_action = action.action_uuid();
        }
        auto revision =
            detail::advance_revision(writer_, {.kind = "edit",
                                               .structure_changed = planner.effects().structure_changed,
                                               .details = {{"action", uuids::to_string(action.action_uuid())}}});
        if (!revision) return std::unexpected(revision.error());
        if (!operations.empty())
            if (auto chunked = action.add_chunk(operations, *revision); !chunked) return chunked;
        if (last_chunk && action.exists())
            if (auto finished = action.finish(); !finished) return finished;
        MutationReceipt next = receipt;
        next.committed_revision = *revision;
        next.applied = planner.applied_count();
        next.skipped = planner.skipped_count();
        const std::size_t next_offset = static_cast<std::size_t>(chunk.data() - all_samples.data()) + chunk.size();
        if (auto saved = store_receipt(writer_, next, digest, last_chunk ? "completed" : "running",
                                       action.recorded_action_id(), next_offset);
            !saved)
            return saved;
        if (auto committed = commit_revision(transaction, *revision); !committed) return committed;
        prepared = true;
        committed_any = true;
        receipt = next;
        return {};
    };

    for (; offset < all_samples.size(); offset += EditCommand::kSamplesPerChunk) {
        const auto chunk =
            all_samples.subspan(offset, std::min(EditCommand::kSamplesPerChunk, all_samples.size() - offset));
        const bool last_chunk = offset + chunk.size() >= all_samples.size();
        if (auto committed = commit_chunk(chunk, last_chunk); !committed) {
            if (committed_any || resuming) {
                // Earlier chunks stay committed: the action and its receipt become partial (a retry
                // with the same operation id resumes here) instead of looking applied or running.
                sqlite::WriteTransaction transaction(writer_);
                auto marked = transaction.begin();
                if (marked && action.exists()) marked = action.mark_partial();
                if (marked)
                    marked = store_receipt(writer_, receipt, digest, "partial", action.recorded_action_id(), offset);
                if (marked) marked = transaction.commit();
                if (!marked) {
                    Error error = committed.error();
                    error.safe_detail +=
                        std::format(" (and recording the partial edit failed: {})", marked.error().safe_detail);
                    return std::unexpected(std::move(error));
                }
            }
            return std::unexpected(committed.error());
        }
    }
    return receipt;
}

Result<MutationReceipt> Library::apply_taxonomy(const TaxonomyCommand& command) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    const std::string digest = command_digest(command);
    const std::lock_guard lock(writer_mutex_);
    auto stored = find_receipt(writer_, command.operation, digest);
    if (!stored) return std::unexpected(stored.error());
    if (*stored) return (*stored)->receipt;

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

    auto revision =
        detail::advance_revision(writer_, {.kind = "taxonomy",
                                           .structure_changed = true,
                                           .details = {{"action", uuids::to_string(action.action_uuid())}}});
    if (!revision) return std::unexpected(revision.error());
    if (auto chunked = action.add_chunk(planner.take_operations(), *revision); !chunked)
        return std::unexpected(chunked.error());
    if (auto finished = action.finish(); !finished) return std::unexpected(finished.error());
    const MutationReceipt receipt{.operation = command.operation,
                                  .committed_revision = *revision,
                                  .history_action = action.action_uuid(),
                                  .applied = 1};
    if (auto saved = store_receipt(writer_, receipt, digest, "completed", action.action_id()); !saved)
        return std::unexpected(saved.error());
    if (auto committed = commit_revision(transaction, *revision); !committed) return std::unexpected(committed.error());
    return receipt;
}

// ---------------------------------------------------------------- undo / redo

namespace {

/// The action an undo or redo would step over.
struct HistoryTarget {
    std::int64_t id = 0;
    uuids::uuid uuid;
    bool completed = false;  ///< all chunks were applied (completed_at set), as opposed to partial
};

/// Applies all chunks of an action in one direction, inside the caller's transaction.
Result<std::int64_t> replay_action(sqlite::Connection& connection, std::int64_t action_id, PatchDirection direction,
                                   PatchEffects& effects) {
    const bool undoing = direction == PatchDirection::Inverse;
    auto chunks = connection.prepare(
        undoing ? "SELECT chunk_index, forward_json FROM history_chunks WHERE action_id=?1 AND state='applied' "
                  "ORDER BY chunk_index DESC"
                : "SELECT chunk_index, forward_json FROM history_chunks WHERE action_id=?1 AND state='undone' "
                  "ORDER BY chunk_index ASC");
    if (!chunks) return std::unexpected(chunks.error());
    chunks->bind(1, action_id);
    std::vector<std::pair<std::int64_t, nlohmann::json>> loaded;
    auto walked = detail::for_each_row(*chunks, [&](sqlite::PreparedStatement& row) -> Result<void> {
        auto operations = json_fields::parse_json(row.column_text(1));
        if (!operations.is_array())
            return fail(ErrorCode::DbCorrupt,
                        std::format("history chunk {} of action {} is malformed", row.column_int64(0), action_id));
        loaded.emplace_back(row.column_int64(0), std::move(operations));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());

    auto mark = connection.prepare("UPDATE history_chunks SET state=?3 WHERE action_id=?1 AND chunk_index=?2");
    if (!mark) return std::unexpected(mark.error());
    std::int64_t operations_applied = 0;
    for (const auto& [index, operations] : loaded) {
        if (undoing) {
            for (auto operation = operations.rbegin(); operation != operations.rend(); ++operation) {
                if (auto applied = detail::apply_operation(connection, *operation, direction, effects); !applied)
                    return std::unexpected(applied.error());
                ++operations_applied;
            }
        } else {
            for (const auto& operation : operations) {
                if (auto applied = detail::apply_operation(connection, operation, direction, effects); !applied)
                    return std::unexpected(applied.error());
                ++operations_applied;
            }
        }
        mark->bind(1, action_id).bind(2, index).bind(3, undoing ? "undone" : "applied");
        if (auto marked = mark->run(); !marked) return std::unexpected(marked.error());
    }
    return operations_applied;
}

/// Finds the action at the cursor (undo) or the oldest undone action (redo) and checks that this
/// build can replay its payload version.
Result<HistoryTarget> find_history_target(sqlite::Connection& connection, bool undoing) {
    std::optional<std::int64_t> id;
    if (undoing) {
        auto cursor = cursor_action(connection);
        if (!cursor) return std::unexpected(cursor.error());
        id = *cursor;
    } else {
        auto next = connection.prepare("SELECT id FROM history_actions WHERE state='undone' ORDER BY id LIMIT 1");
        if (!next) return std::unexpected(next.error());
        auto has_row = next->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (*has_row) id = next->column_int64(0);
        next->reset();
    }
    if (!id) return fail(ErrorCode::NotFound, undoing ? "nothing to undo" : "nothing to redo");

    auto info = connection.prepare(
        "SELECT uuid, state, command_version, completed_at IS NOT NULL FROM history_actions WHERE id=?1");
    if (!info) return std::unexpected(info.error());
    info->bind(1, *id);
    auto has_row = info->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::DbCorrupt, "the history cursor points at a missing action");
    const std::string uuid_text = info->column_text(0);
    const std::string state = info->column_text(1);
    const auto version = info->column_int64(2);
    const bool completed = info->column_int64(3) != 0;
    info->reset();
    if (undoing && state != "applied" && state != "partial") return fail(ErrorCode::NotFound, "nothing to undo");
    if (version > detail::kHistoryCommandVersion)
        return fail(ErrorCode::SchemaTooNew, "this history entry was written by a newer version of the application");
    if (version < detail::kOldestReadableHistoryCommandVersion)
        return fail(ErrorCode::DbCorrupt, std::format("history entry version {} is not supported", version));
    auto uuid = detail::parse_stored_uuid(uuid_text, "history action");
    if (!uuid) return std::unexpected(uuid.error());
    return HistoryTarget{*id, *uuid, completed};
}

}  // namespace

Result<MutationReceipt> Library::undo(const uuids::uuid& operation) {
    return step_history(operation, /*undoing=*/true);
}

Result<MutationReceipt> Library::redo(const uuids::uuid& operation) {
    return step_history(operation, /*undoing=*/false);
}

Result<MutationReceipt> Library::step_history(const uuids::uuid& operation, bool undoing) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    const std::string digest = history_step_digest(undoing);
    const std::lock_guard lock(writer_mutex_);
    auto stored = find_receipt(writer_, operation, digest);
    if (!stored) return std::unexpected(stored.error());
    if (*stored) {  // a retried undo never reverts a second action
        if ((*stored)->state == "failed")
            return fail(ErrorCode::HistoryConflict, "this operation already ended in a history conflict");
        return (*stored)->receipt;
    }

    HistoryTarget target;
    Error conflict;
    {
        sqlite::WriteTransaction transaction(writer_);
        if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
        auto found = find_history_target(writer_, undoing);
        if (!found) return std::unexpected(found.error());
        target = *found;

        PatchEffects effects;
        auto applied =
            replay_action(writer_, target.id, undoing ? PatchDirection::Inverse : PatchDirection::Forward, effects);
        if (applied) {
            const std::string_view new_state = undoing ? "undone" : (target.completed ? "applied" : "partial");
            auto mark = writer_.prepare("UPDATE history_actions SET state=?2 WHERE id=?1");
            if (!mark) return std::unexpected(mark.error());
            mark->bind(1, target.id).bind(2, new_state);
            if (auto marked = mark->run(); !marked) return std::unexpected(marked.error());
            auto move = writer_.prepare(
                undoing ? "UPDATE history_state SET cursor_action_id=(SELECT max(id) FROM history_actions "
                          "WHERE id<?1 AND state IN ('applied','partial'))"
                        : "UPDATE history_state SET cursor_action_id=?1");
            if (!move) return std::unexpected(move.error());
            move->bind(1, target.id);
            if (auto moved = move->run(); !moved) return std::unexpected(moved.error());
            auto revision = detail::advance_revision(writer_, {.kind = undoing ? "undo" : "redo",
                                                               .structure_changed = effects.structure_changed,
                                                               .details = {{"action", uuids::to_string(target.uuid)}}});
            if (!revision) return std::unexpected(revision.error());
            const MutationReceipt receipt{.operation = operation,
                                          .committed_revision = *revision,
                                          .history_action = target.uuid,
                                          .applied = *applied};
            if (auto saved = store_receipt(writer_, receipt, digest, "completed", target.id); !saved)
                return std::unexpected(saved.error());
            if (auto committed = commit_revision(transaction, *revision); !committed)
                return std::unexpected(committed.error());
            return receipt;
        }
        if (applied.error().code != ErrorCode::HistoryConflict) return std::unexpected(applied.error());
        conflict = applied.error();
    }  // the failed attempt rolls back here

    // Record the conflict once: the action leaves the undo/redo path (state 'conflict'), so retrying
    // neither repeats the review item nor blocks older actions.
    sqlite::WriteTransaction transaction(writer_);
    auto recorded = transaction.begin();
    if (recorded) recorded = run_with_id(writer_, "UPDATE history_actions SET state='conflict' WHERE id=?1", target.id);
    if (recorded)
        recorded = run_with_id(writer_,
                               undoing ? "UPDATE history_state SET cursor_action_id=(SELECT max(id) FROM "
                                         "history_actions WHERE id<?1 AND state IN ('applied','partial'))"
                                       : "UPDATE history_actions SET state='superseded' WHERE state='undone' AND id>?1",
                               target.id);
    if (recorded)
        recorded = detail::add_review_item(writer_, "history_conflict", std::nullopt,
                                           nlohmann::json{{"action_id", target.id},
                                                          {"action", uuids::to_string(target.uuid)},
                                                          {"direction", undoing ? "undo" : "redo"},
                                                          {"detail", conflict.safe_detail}});
    if (recorded) {
        auto revision = detail::advance_revision(
            writer_, {.kind = "history_conflict", .details = {{"action", uuids::to_string(target.uuid)}}});
        // The receipt remembers the outcome, so resending the same operation reports the conflict
        // again instead of undoing (or redoing) the next action.
        if (revision)
            recorded = store_receipt(writer_,
                                     MutationReceipt{.operation = operation,
                                                     .committed_revision = *revision,
                                                     .history_action = target.uuid,
                                                     .conflicted = 1},
                                     digest, "failed", target.id);
        else
            recorded = std::unexpected(revision.error());
        if (recorded) recorded = commit_revision(transaction, *revision);
    }
    if (!recorded)
        conflict.safe_detail += std::format(" (and recording the conflict failed: {})", recorded.error().safe_detail);
    return std::unexpected(std::move(conflict));
}

Result<std::vector<HistoryEntry>> Library::history(std::size_t limit) {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto statement = reader->connection().prepare(
        "SELECT action.id, action.uuid, action.label, action.state, action.created_at, "
        "(action.id IS history.cursor_action_id) FROM history_actions action, history_state history "
        "WHERE action.state<>'superseded' ORDER BY action.id DESC LIMIT ?1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, static_cast<std::int64_t>(std::min<std::size_t>(limit, 100'000)));
    std::vector<HistoryEntry> entries;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        auto uuid = detail::parse_stored_uuid(row.column_text(1), "history action");
        if (!uuid) return std::unexpected(uuid.error());
        entries.push_back(HistoryEntry{row.column_int64(0), *uuid, row.column_text(2), row.column_text(3),
                                       row.column_int64(4), row.column_int64(5) != 0});
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return entries;
}

}  // namespace mso::catalog
