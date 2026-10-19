// Catalog: identity, migrations, edits, durable undo/redo, conflicts, idempotency, taxonomy, scans,
// root availability, the change feed, crash recovery, backups and the analysis queue.
#include <doctest.h>

#include <algorithm>
#include <format>

#include <nlohmann/json.hpp>

#include "mso/catalog/detail/library_test_access.hpp"
#include "mso/catalog/durable_file.hpp"
#include "mso/catalog/library.hpp"
#include "mso/catalog/migrations.hpp"
#include "mso/catalog/root_paths.hpp"
#include "test_support.hpp"

using namespace mso;
using namespace mso::catalog;
using mso::test::ScratchDirectory;

namespace {

std::vector<ScannedFile> three_samples() {
    return {test::synthetic_file("Drums/Kick 01.wav", 1000, 400'000),
            test::synthetic_file("Drums/Snare.wav", 2000, 300'000),
            test::synthetic_file("Loops/Groove 128.wav", 3000, 8'000'000)};
}

EditCommand rate(std::vector<std::int64_t> samples, int rating) {
    EditCommand command;
    command.sample_ids = std::move(samples);
    command.rating = rating;
    return command;
}

sqlite::Connection& writer(Library& library) {
    return LibraryTestAccess::writer(library);
}

/// Runs a statement on the writer (test-only fault injection); fails the test on error.
void execute(Library& library, const std::string& sql) {
    auto executed = writer(library).execute_script(sql);
    REQUIRE_MESSAGE(executed.has_value(), executed.error().message());
}

/// First column of the first row of a query on the writer.
std::int64_t query(Library& library, const std::string& sql) {
    auto value = writer(library).query_int64(sql);
    REQUIRE_MESSAGE(value.has_value(), value.error().message());
    return *value;
}

std::string query_text(Library& library, const std::string& sql) {
    auto statement = writer(library).prepare(sql);
    REQUIRE(statement.has_value());
    auto has_row = statement->next_row();
    REQUIRE(has_row.has_value());
    REQUIRE(*has_row);
    return statement->column_text(0);
}

/// Every sample id of the library, in id order (tests never assume ids are 1..N).
std::vector<std::int64_t> all_sample_ids(Library& library) {
    auto reader = library.acquire_reader();
    REQUIRE(reader.has_value());
    auto statement = reader->connection().prepare("SELECT id FROM samples ORDER BY id");
    REQUIRE(statement.has_value());
    std::vector<std::int64_t> ids;
    while (true) {
        auto has_row = statement->next_row();
        REQUIRE(has_row.has_value());
        if (!*has_row) break;
        ids.push_back(statement->column_int64(0));
    }
    return ids;
}

std::int64_t current_revision(Library& library) {
    auto revision = library.catalog_revision();
    REQUIRE(revision.has_value());
    return *revision;
}

/// Adds a root and scans `files` into it.
std::int64_t add_scanned_root(Library& library, std::string_view name, const std::vector<ScannedFile>& files,
                              std::optional<bool> case_sensitive = std::nullopt) {
    auto root = library.add_root(name, RootMode::Referenced, case_sensitive);
    REQUIRE(root.has_value());
    auto session = library.begin_scan(root->id);
    REQUIRE(session.has_value());
    REQUIRE(library.commit_scan_batch(*session, files, {}, [](std::string_view) { return false; }).has_value());
    REQUIRE(library.finish_scan(*session, false).has_value());
    return root->id;
}

std::string job_state(Library& library, std::int64_t job_id) {
    return query_text(library, std::format("SELECT state FROM jobs WHERE id={}", job_id));
}

LocationState location_state(Library& library, std::int64_t sample_id) {
    auto detail = library.sample_detail(sample_id);
    REQUIRE(detail.has_value());
    REQUIRE_FALSE(detail->locations.empty());
    return detail->locations.front().state;
}

/// Writes a catalog with only migrations 1–4 (as released before this build) plus its manifest.
void write_version_four_library(const std::filesystem::path& directory, const uuids::uuid& uuid) {
    std::filesystem::create_directories(directory);
    auto connection = sqlite::Connection::open(directory / Library::kCatalogFileName, {.create = true});
    REQUIRE(connection.has_value());
    for (const auto& migration : migrations()) {
        if (migration.version > 4) break;
        REQUIRE(connection->execute_script(migration.sql).has_value());
        auto record = connection->prepare("INSERT INTO schema_migrations(version,checksum,applied_at) VALUES(?1,?2,0)");
        REQUIRE(record.has_value());
        record->bind(1, migration.version).bind(2, migration.checksum);
        REQUIRE(record->run().has_value());
        if (migration.version == 1) {
            auto state = connection->prepare(
                "INSERT INTO library_state(singleton,library_uuid,name,schema_version,catalog_revision,created_at,"
                "normalization_version) VALUES(1,?1,'Old',1,0,0,?2)");
            REQUIRE(state.has_value());
            state->bind(1, uuids::to_string(uuid)).bind(2, kFoldVersion);
            REQUIRE(state->run().has_value());
        }
    }
    REQUIRE(connection->execute_script("UPDATE library_state SET schema_version=4; PRAGMA user_version=4").has_value());
    connection->close();
    const nlohmann::json manifest = {{"format_version", 1}, {"library_uuid", uuids::to_string(uuid)}, {"name", "Old"}};
    REQUIRE(write_file_atomically(directory / Library::kManifestFileName, manifest.dump()).has_value());
}

}  // namespace

TEST_SUITE("catalog") {
    // ------------------------------------------------------------ identity, schema, lifecycle

    TEST_CASE("a new library has identity, all migrations and a healthy database") {
        ScratchDirectory scratch;
        auto library = Library::create(scratch / "lib", "Mine");
        REQUIRE(library.has_value());
        const auto identity = (*library)->identity();
        CHECK(identity.name == "Mine");
        CHECK_FALSE(identity.uuid.is_nil());
        CHECK(identity.schema_version == latest_schema_version());
        CHECK(query(**library, "PRAGMA user_version") == latest_schema_version());
        CHECK(std::filesystem::exists(scratch / "lib/library.json"));
        auto report = (*library)->integrity_report();
        REQUIRE(report.has_value());
        CHECK(report->find("integrity: ok; foreign keys: 0") == 0);
    }

    TEST_CASE("released migrations are pinned: their checksums never change") {
        // Invariant 15: editing a released migration breaks every existing library. Add a new one.
        const std::vector<std::pair<int, std::string_view>> released = {
            {1, "a37f78a4c237f749d671c23615f9d8186badb34fdc14cd9b9bfd3767eff6f8bc"},
            {2, "4420190f3b364b5c96c6c9f9fd417ce3382c5e21e6eb981ae9ce9870896cbb7d"},
            {3, "9122c1c8f2c8ae370655ed299a16ae3391af06cb9d55d435605ff6c7c11e2e76"},
            {4, "f6dd240e6d183500cad2f7621f330ff4ae438230550141404e13fae57a00f128"},
            {5, "c892cdf8794da18f988447b87f103a5933ca5ba50c61d73ddd839a8b04dbe7b4"},
        };
        REQUIRE(migrations().size() >= released.size());
        for (const auto& [version, checksum] : released) {
            CAPTURE(version);
            CHECK(migrations()[static_cast<std::size_t>(version - 1)].version == version);
            CHECK(migrations()[static_cast<std::size_t>(version - 1)].checksum == checksum);
        }
    }

    TEST_CASE("creating a library refuses a non-empty folder and leaves it untouched") {
        ScratchDirectory scratch;
        test::write_sine_wav(scratch / "lib/existing.wav", 0.1);
        auto library = Library::create(scratch / "lib", "Mine");
        REQUIRE_FALSE(library.has_value());
        CHECK(library.error().code == ErrorCode::InvalidArgument);
        CHECK_FALSE(std::filesystem::exists(scratch / "lib/catalog.sqlite"));
    }

    TEST_CASE("opening rejects a database that belongs to another library") {
        ScratchDirectory scratch;
        REQUIRE(Library::create(scratch / "a", "A").has_value());
        REQUIRE(Library::create(scratch / "b", "B").has_value());
        std::filesystem::copy_file(scratch / "b/library.json", scratch / "a/library.json",
                                   std::filesystem::copy_options::overwrite_existing);
        auto opened = Library::open(scratch / "a");
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == ErrorCode::DbCorrupt);
    }

    TEST_CASE("a tampered released migration is detected on open") {
        ScratchDirectory scratch;
        {
            auto library = Library::create(scratch / "lib", "Mine");
            REQUIRE(library.has_value());
            execute(**library, "UPDATE schema_migrations SET checksum='bogus' WHERE version=1");
        }
        auto opened = Library::open(scratch / "lib");
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == ErrorCode::DbCorrupt);
    }

    TEST_CASE("a gap in the applied migrations is detected on open") {
        ScratchDirectory scratch;
        {
            auto library = Library::create(scratch / "lib", "Mine");
            REQUIRE(library.has_value());
            execute(**library, "DELETE FROM schema_migrations WHERE version=2");
        }
        auto opened = Library::open(scratch / "lib");
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == ErrorCode::DbCorrupt);
    }

    TEST_CASE("an older schema is upgraded after a verified backup, and cannot be opened read-only") {
        ScratchDirectory scratch;
        const auto uuid = new_uuid();
        write_version_four_library(scratch / "lib", uuid);
        auto read_only = Library::open(scratch / "lib", {.read_only = true});
        REQUIRE_FALSE(read_only.has_value());
        CHECK(read_only.error().code == ErrorCode::InvalidArgument);

        auto library = Library::open(scratch / "lib");
        REQUIRE_MESSAGE(library.has_value(), library.error().message());
        CHECK((*library)->recovery_report().migrated);
        REQUIRE((*library)->recovery_report().pre_migration_backup.has_value());
        CHECK(std::filesystem::exists(*(*library)->recovery_report().pre_migration_backup));
        CHECK((*library)->identity().schema_version == latest_schema_version());
        CHECK(query(**library, "SELECT count(*) FROM schema_migrations") == latest_schema_version());
        CHECK((*library)->add_root("After upgrade", RootMode::Referenced).has_value());
    }

    TEST_CASE("a newer schema opens read-only and refuses writes") {
        ScratchDirectory scratch;
        {
            auto library = Library::create(scratch / "lib", "Mine");
            REQUIRE(library.has_value());
            execute(**library, std::format("INSERT INTO schema_migrations(version,checksum,applied_at) "
                                           "VALUES({},'from the future',0)",
                                           latest_schema_version() + 1));
        }
        auto library = Library::open(scratch / "lib");
        REQUIRE(library.has_value());
        CHECK((*library)->read_only());
        auto root = (*library)->add_root("Nope", RootMode::Referenced);
        REQUIRE_FALSE(root.has_value());
        CHECK(root.error().code == ErrorCode::SchemaTooNew);
        CHECK((*library)->list_roots().has_value());  // browsing still works
        CHECK((*library)->acknowledge_changes("ui", 0).has_value());
    }

    TEST_CASE("crash recovery turns half-applied actions into partial ones and pauses scans") {
        ScratchDirectory scratch;
        {
            auto populated = test::make_library(scratch / "lib", three_samples());
            execute(
                *populated.library,
                std::format("INSERT INTO history_actions(uuid,label,command_version,state,created_at) "
                            "VALUES('{}','crashed',2,'applying',0);"
                            "INSERT INTO scan_sessions(root_id,generation,state,started_at) VALUES({},99,'running',0);",
                            uuids::to_string(new_uuid()), populated.root_id));
        }
        auto library = Library::open(scratch / "lib");
        REQUIRE(library.has_value());
        CHECK((*library)->recovery_report().partial_history_actions == 1);
        CHECK((*library)->recovery_report().interrupted_scans == 1);
    }

    TEST_CASE("verified backups are standalone copies with sanitized names, pruned to the newest ones") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto backup = populated.library->create_backup("../evil name?");
        REQUIRE(backup.has_value());
        const auto file_name = path_to_utf8(backup->filename());
        CHECK(file_name.find("..") == std::string::npos);
        CHECK(file_name.find(' ') == std::string::npos);
        CHECK(backup->parent_path().filename() == "backups");
        {
            auto copy = sqlite::Connection::open(*backup, {.read_only = true, .write_ahead_log = false});
            REQUIRE(copy.has_value());
            CHECK(copy->query_int64("SELECT count(*) FROM samples").value_or(0) == 3);
            CHECK(copy->integrity_check().value_or("") == "ok");
        }  // closed: an open copy could not be pruned on Windows

        for (std::size_t index = 0; index < Library::kMaximumBackups + 2; ++index)
            REQUIRE(populated.library->create_backup("again").has_value());
        std::size_t backups = 0;
        for (const auto& entry : std::filesystem::directory_iterator(scratch / "lib/backups"))
            backups += entry.path().extension() == ".sqlite" ? 1 : 0;
        CHECK(backups == Library::kMaximumBackups);
    }

    TEST_CASE("identity() is served from memory and tracks committed revisions") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto before = library.identity().catalog_revision;
        CHECK(before == current_revision(library));
        REQUIRE(library.apply_edit(rate({test::sample_id_named(library, "Snare")}, 2)).has_value());
        CHECK(library.identity().catalog_revision == before + 1);
    }

    TEST_CASE("the library manifest is replaced atomically and leaves no temporary file") {
        ScratchDirectory scratch;
        const auto target = scratch / "manifest.json";
        REQUIRE(write_file_atomically(target, "first").has_value());
        REQUIRE(write_file_atomically(target, "second, longer").has_value());
        CHECK(test::read_file(target) == "second, longer");
        for (const auto& entry : std::filesystem::directory_iterator(scratch.path()))
            CHECK(entry.path().filename() == "manifest.json");
        auto missing_folder = write_file_atomically(scratch / "no/such/folder/file.json", "x");
        CHECK_FALSE(missing_folder.has_value());
    }

    // ------------------------------------------------------------ reader pool

    TEST_CASE("the reader pool times out instead of deadlocking, and leases outlive the library") {
        ScratchDirectory scratch;
        {
            auto populated = test::make_library(scratch / "lib", three_samples());
        }
        auto library = Library::open(scratch / "lib", {.max_readers = 1});
        REQUIRE(library.has_value());
        auto first = (*library)->acquire_reader();
        REQUIRE(first.has_value());
        auto second = (*library)->acquire_reader(std::chrono::milliseconds(30));
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == ErrorCode::ResourceLimit);
        CHECK(second.error().retryable);

        library->reset();  // the lease keeps its connection usable
        CHECK(first->connection().query_int64("SELECT count(*) FROM samples").value_or(-1) == 3);
    }

    // ------------------------------------------------------------ edits and history

    TEST_CASE("edits, undo and redo survive closing and reopening the library") {
        ScratchDirectory scratch;
        std::int64_t kick = 0;
        {
            auto populated = test::make_library(scratch / "lib", three_samples());
            kick = test::sample_id_named(*populated.library, "Kick 01");
            EditCommand edit = rate({kick}, 4);
            edit.favorite = true;
            edit.tags_add = {"Drums/Kick"};
            edit.notes = "punchy";
            REQUIRE(populated.library->apply_edit(edit).has_value());
        }
        {
            auto library = Library::open(scratch / "lib");
            REQUIRE(library.has_value());
            auto detail = (*library)->sample_detail(kick);
            REQUIRE(detail.has_value());
            CHECK(detail->rating == 4);
            CHECK(detail->favorite);
            CHECK(detail->notes == "punchy");
            REQUIRE(detail->tags.size() == 1);
            CHECK(detail->tags.front().path == "Drums/Kick");
            REQUIRE((*library)->undo().has_value());
        }
        {
            auto library = Library::open(scratch / "lib");
            REQUIRE(library.has_value());
            auto detail = (*library)->sample_detail(kick);
            REQUIRE(detail.has_value());
            CHECK(detail->rating == 0);
            CHECK_FALSE(detail->favorite);
            CHECK(detail->tags.empty());
            REQUIRE((*library)->redo().has_value());
            CHECK((*library)->sample_detail(kick)->rating == 4);
            auto nothing = (*library)->redo();
            REQUIRE_FALSE(nothing.has_value());
            CHECK(nothing.error().code == ErrorCode::NotFound);
        }
    }

    TEST_CASE("tags and categories created by an edit are removed again by its undo") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        EditCommand edit;
        edit.sample_ids = {test::sample_id_named(library, "Kick 01")};
        edit.tags_add = {"New/Tag"};
        edit.category = std::optional<std::string>("Fresh/Category");
        REQUIRE(library.apply_edit(edit).has_value());
        CHECK(library.list_tags()->size() == 2);
        CHECK(library.list_categories()->size() == 2);
        REQUIRE(library.undo().has_value());
        CHECK(library.list_tags()->empty());
        CHECK(library.list_categories()->empty());
        REQUIRE(library.redo().has_value());
        CHECK(library.list_tags()->size() == 2);
    }

    TEST_CASE("an undo conflict marks the action, moves the cursor past it and records one review item") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto snare = test::sample_id_named(library, "Snare");
        const auto kick = test::sample_id_named(library, "Kick 01");
        REQUIRE(library.apply_edit(rate({kick}, 1)).has_value());
        REQUIRE(library.apply_edit(rate({snare}, 3)).has_value());
        // A later change outside the history cursor (e.g. another tool) changes the same field.
        execute(library, std::format("UPDATE samples SET rating=5 WHERE id={}", snare));
        auto undone = library.undo();
        REQUIRE_FALSE(undone.has_value());
        CHECK(undone.error().code == ErrorCode::HistoryConflict);
        CHECK(library.sample_detail(snare)->rating == 5);  // nothing overwritten

        auto history = library.history();
        REQUIRE(history.has_value());
        REQUIRE(history->size() == 2);
        CHECK(history->front().state == "conflict");
        CHECK_FALSE(history->front().is_cursor);
        CHECK(history->back().is_cursor);

        // The older action is still undoable, and retrying does not add review items.
        REQUIRE(library.undo().has_value());
        CHECK(library.sample_detail(kick)->rating == 0);
        auto review = library.open_review_items();
        REQUIRE(review.has_value());
        CHECK(std::ranges::count(*review, std::string("history_conflict"), &ReviewItem::kind) == 1);
    }

    TEST_CASE("a retried undo with the same operation id does not undo a second action") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        REQUIRE(library.apply_edit(rate({kick}, 1)).has_value());
        REQUIRE(library.apply_edit(rate({kick}, 2)).has_value());
        const auto operation = new_uuid();
        auto first = library.undo(operation);
        REQUIRE(first.has_value());
        auto retried = library.undo(operation);
        REQUIRE(retried.has_value());
        CHECK(retried->replayed);
        CHECK(retried->committed_revision == first->committed_revision);
        CHECK(library.sample_detail(kick)->rating == 1);
        auto reused_for_redo = library.redo(operation);
        REQUIRE_FALSE(reused_for_redo.has_value());
        CHECK(reused_for_redo.error().code == ErrorCode::IdempotencyMismatch);
    }

    TEST_CASE("a new edit after undo supersedes the redo tail") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        REQUIRE(library.apply_edit(rate({kick}, 1)).has_value());
        REQUIRE(library.undo().has_value());
        REQUIRE(library.apply_edit(rate({kick}, 2)).has_value());
        auto redo = library.redo();
        REQUIRE_FALSE(redo.has_value());
        CHECK(library.sample_detail(kick)->rating == 2);
    }

    TEST_CASE("replaying an operation id returns the stored receipt; reusing it differently is rejected") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        EditCommand edit = rate({kick}, 5);
        auto first = library.apply_edit(edit);
        auto second = library.apply_edit(edit);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(second->replayed);
        CHECK_FALSE(second->partial);
        CHECK(second->committed_revision == first->committed_revision);
        edit.rating = 1;
        auto mismatch = library.apply_edit(edit);
        REQUIRE_FALSE(mismatch.has_value());
        CHECK(mismatch.error().code == ErrorCode::IdempotencyMismatch);
        CHECK(library.sample_detail(kick)->rating == 5);
    }

    TEST_CASE("invalid edits are rejected before anything is written") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        const auto revision = current_revision(library);
        CHECK(library.apply_edit(rate({kick}, 6)).error().code == ErrorCode::InvalidArgument);
        EditCommand both;
        both.sample_ids = {kick};
        both.tags_add = {"Drums / Kick"};
        both.tags_remove = {"drums/kick"};  // the same tag spelled differently
        CHECK(library.apply_edit(both).error().code == ErrorCode::InvalidArgument);
        EditCommand deep;
        deep.sample_ids = {kick};
        deep.tags_add = {"1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17"};
        CHECK(library.apply_edit(deep).error().code == ErrorCode::InvalidArgument);
        CHECK(current_revision(library) == revision);
    }

    TEST_CASE("a bulk edit larger than one chunk is one undoable action") {
        ScratchDirectory scratch;
        std::vector<ScannedFile> files;
        for (int index = 0; index < 1203; ++index)
            files.push_back(test::synthetic_file(std::format("bulk/{:04}.wav", index), 100 + index, 1'000'000));
        auto populated = test::make_library(scratch / "lib", files);
        auto& library = *populated.library;
        EditCommand edit;
        edit.sample_ids = all_sample_ids(library);
        REQUIRE(edit.sample_ids.size() == 1203);
        edit.tags_add = {"Bulk"};
        auto receipt = library.apply_edit(edit);
        REQUIRE(receipt.has_value());
        CHECK(receipt->applied == 1203);
        CHECK_FALSE(receipt->partial);
        auto history = library.history();
        REQUIRE(history.has_value());
        CHECK(history->size() == 1);
        CHECK(history->front().state == "applied");
        REQUIRE(library.undo().has_value());
        CHECK(library.list_tags()->empty());  // the implicitly created tag is gone with the links
    }

    TEST_CASE("a chunked edit that fails mid-way is partial, and a retry resumes it") {
        ScratchDirectory scratch;
        std::vector<ScannedFile> files;
        for (int index = 0; index < 1100; ++index)
            files.push_back(test::synthetic_file(std::format("bulk/{:04}.wav", index), 100 + index, 1'000'000));
        auto populated = test::make_library(scratch / "lib", files);
        auto& library = *populated.library;
        EditCommand edit;
        edit.sample_ids = all_sample_ids(library);
        edit.tags_add = {"Bulk"};
        const auto failing_sample = edit.sample_ids[700];  // inside the second chunk
        execute(library, std::format("CREATE TEMP TRIGGER inject_failure BEFORE INSERT ON sample_tags "
                                     "WHEN new.sample_id={} BEGIN SELECT RAISE(ABORT, 'injected failure'); END",
                                     failing_sample));
        auto failed = library.apply_edit(edit);
        REQUIRE_FALSE(failed.has_value());
        auto history = library.history();
        REQUIRE(history.has_value());
        REQUIRE(history->size() == 1);
        CHECK(history->front().state == "partial");
        CHECK(query(library, "SELECT count(*) FROM sample_tags") == 500);
        CHECK(query_text(library, std::format("SELECT state FROM command_receipts WHERE operation_uuid='{}'",
                                              uuids::to_string(edit.operation))) == "partial");

        execute(library, "DROP TRIGGER temp.inject_failure");
        auto resumed = library.apply_edit(edit);
        REQUIRE_MESSAGE(resumed.has_value(), resumed.error().message());
        CHECK_FALSE(resumed->replayed);
        CHECK_FALSE(resumed->partial);
        CHECK(resumed->applied == 1100);
        CHECK(query(library, "SELECT count(*) FROM sample_tags") == 1100);
        history = library.history();
        REQUIRE(history->size() == 1);
        CHECK(history->front().state == "applied");
        auto replayed = library.apply_edit(edit);
        REQUIRE(replayed.has_value());
        CHECK(replayed->replayed);

        REQUIRE(library.undo().has_value());
        CHECK(query(library, "SELECT count(*) FROM sample_tags") == 0);
    }

    TEST_CASE("a partial edit that can no longer resume is reported as partial, never as success") {
        ScratchDirectory scratch;
        std::vector<ScannedFile> files;
        for (int index = 0; index < 600; ++index)
            files.push_back(test::synthetic_file(std::format("bulk/{:04}.wav", index), 100 + index, 1'000'000));
        auto populated = test::make_library(scratch / "lib", files);
        auto& library = *populated.library;
        EditCommand edit;
        edit.sample_ids = all_sample_ids(library);
        edit.favorite = true;
        execute(library, std::format("CREATE TEMP TRIGGER inject_failure BEFORE UPDATE OF favorite ON samples "
                                     "WHEN new.id={} BEGIN SELECT RAISE(ABORT, 'injected failure'); END",
                                     edit.sample_ids.back()));
        REQUIRE_FALSE(library.apply_edit(edit).has_value());
        execute(library, "DROP TRIGGER temp.inject_failure");
        REQUIRE(library.apply_edit(rate({edit.sample_ids.front()}, 3)).has_value());  // newer history

        auto retried = library.apply_edit(edit);
        REQUIRE(retried.has_value());
        CHECK(retried->replayed);
        CHECK(retried->partial);
        CHECK(retried->applied == 500);
    }

    TEST_CASE("corrupt or newer history is reported, never thrown") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        REQUIRE(library.apply_edit(rate({test::sample_id_named(library, "Kick 01")}, 2)).has_value());
        execute(library, "UPDATE history_actions SET command_version=99");
        auto newer = library.undo();
        REQUIRE_FALSE(newer.has_value());
        CHECK(newer.error().code == ErrorCode::SchemaTooNew);

        execute(library,
                "UPDATE history_actions SET command_version=2; "
                "UPDATE history_chunks SET forward_json='[{\"op\":\"field\",\"field\":7}]'");
        auto corrupt = library.undo();
        REQUIRE_FALSE(corrupt.has_value());
        CHECK(corrupt.error().code == ErrorCode::DbCorrupt);
    }

    // ------------------------------------------------------------ taxonomy and collections

    TEST_CASE("merging tags is undoable and restores the exact original memberships") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        const auto snare = test::sample_id_named(library, "Snare");
        EditCommand tag_both;
        tag_both.sample_ids = {kick, snare};
        tag_both.tags_add = {"Percussion"};
        REQUIRE(library.apply_edit(tag_both).has_value());
        EditCommand tag_kick;
        tag_kick.sample_ids = {kick};
        tag_kick.tags_add = {"Drums"};
        REQUIRE(library.apply_edit(tag_kick).has_value());
        auto tags = library.list_tags();
        REQUIRE(tags.has_value());
        const auto percussion = std::ranges::find(*tags, std::string("Percussion"), &TaxonomyNode::name)->id;
        const auto drums = std::ranges::find(*tags, std::string("Drums"), &TaxonomyNode::name)->id;

        TaxonomyCommand merge{.kind = TaxonomyCommand::Kind::MergeTag, .target_id = percussion, .other_id = drums};
        REQUIRE(library.apply_taxonomy(merge).has_value());
        auto merged = library.list_tags();
        REQUIRE(merged->size() == 1);
        CHECK(merged->front().sample_count == 2);
        EditCommand alias_use;  // the old name now resolves to the canonical tag
        alias_use.sample_ids = {test::sample_id_named(library, "Groove 128")};
        alias_use.tags_add = {"Percussion"};
        REQUIRE(library.apply_edit(alias_use).has_value());
        CHECK(library.sample_detail(alias_use.sample_ids.front())->tags.front().name == "Drums");
        REQUIRE(library.undo().has_value());

        REQUIRE(library.undo().has_value());  // undo the merge
        auto restored = library.list_tags();
        REQUIRE(restored->size() == 2);
        auto kick_tags = library.sample_detail(kick)->tags;
        auto snare_tags = library.sample_detail(snare)->tags;
        CHECK(kick_tags.size() == 2);  // kept its pre-existing Drums link
        REQUIRE(snare_tags.size() == 1);
        CHECK(snare_tags.front().name == "Percussion");
    }

    TEST_CASE("tag moves reject cycles, missing parents and moves that make the tree too deep") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        auto parent = library.ensure_tag("Drums");
        auto child = library.ensure_tag("Drums/Kick");
        REQUIRE(parent.has_value());
        REQUIRE(child.has_value());
        TaxonomyCommand move{.kind = TaxonomyCommand::Kind::MoveTag, .target_id = *parent, .new_parent_id = *child};
        auto moved = library.apply_taxonomy(move);
        REQUIRE_FALSE(moved.has_value());
        CHECK(moved.error().code == ErrorCode::InvalidArgument);

        TaxonomyCommand orphan{.kind = TaxonomyCommand::Kind::MoveTag, .target_id = *child, .new_parent_id = 999'999};
        CHECK(library.apply_taxonomy(orphan).error().code == ErrorCode::NotFound);

        auto deep = library.ensure_tag("A/B/C/D/E/F/G/H/I/J/K/L/M/N/O");  // 15 levels
        REQUIRE(deep.has_value());
        // Drums/Kick is two levels: under a 15-level node it would reach 17.
        TaxonomyCommand too_deep{.kind = TaxonomyCommand::Kind::MoveTag, .target_id = *parent, .new_parent_id = *deep};
        CHECK(library.apply_taxonomy(too_deep).error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("ensure_tag commits a revision only when it creates something") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        auto created = library.ensure_tag("Drums");
        REQUIRE(created.has_value());
        const auto revision = current_revision(library);
        auto existing = library.ensure_tag(" drums ");
        REQUIRE(existing.has_value());
        CHECK(*existing == *created);
        CHECK(current_revision(library) == revision);
    }

    TEST_CASE("renaming a category is undoable and refreshes the search projection") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        EditCommand categorize;
        categorize.sample_ids = {kick};
        categorize.category = std::optional<std::string>("Drums/Kick");
        REQUIRE(library.apply_edit(categorize).has_value());
        auto categories = library.list_categories();
        REQUIRE(categories.has_value());
        const auto drums = std::ranges::find(*categories, std::string("Drums"), &TaxonomyNode::name)->id;

        TaxonomyCommand rename{
            .kind = TaxonomyCommand::Kind::RenameCategory, .target_id = drums, .new_name = "Percussion"};
        REQUIRE(library.apply_taxonomy(rename).has_value());
        CHECK(library.sample_detail(kick)->category_path == "Percussion/Kick");
        CHECK(query_text(library, std::format("SELECT category FROM sample_search WHERE sample_id={}", kick)) ==
              "Percussion/Kick");
        REQUIRE(library.undo().has_value());
        CHECK(library.sample_detail(kick)->category_path == "Drums/Kick");
        CHECK(query_text(library, std::format("SELECT category FROM sample_search WHERE sample_id={}", kick)) ==
              "Drums/Kick");

        TaxonomyCommand missing{.kind = TaxonomyCommand::Kind::RenameCategory, .target_id = 999'999, .new_name = "X"};
        CHECK(library.apply_taxonomy(missing).error().code == ErrorCode::NotFound);
    }

    TEST_CASE("manual collections keep order and smart collections store their query") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        TaxonomyCommand create{.kind = TaxonomyCommand::Kind::CreateCollection, .new_name = "Crate"};
        REQUIRE(library.apply_taxonomy(create).has_value());
        TaxonomyCommand smart{
            .kind = TaxonomyCommand::Kind::CreateCollection, .new_name = "Long", .smart_query = "duration:>5s"};
        REQUIRE(library.apply_taxonomy(smart).has_value());
        auto collections = library.list_collections();
        REQUIRE(collections->size() == 2);
        const auto crate = std::ranges::find(*collections, std::string("Crate"), &CollectionRecord::name)->id;
        const auto long_collection = std::ranges::find(*collections, std::string("Long"), &CollectionRecord::name);
        CHECK(long_collection->query == std::optional<std::string>("duration:>5s"));
        EditCommand add;
        add.sample_ids = {test::sample_id_named(library, "Snare"), test::sample_id_named(library, "Kick 01")};
        add.add_to_collection = crate;
        REQUIRE(library.apply_edit(add).has_value());
        const auto refreshed = library.list_collections();
        REQUIRE(refreshed.has_value());
        CHECK(std::ranges::find(*refreshed, crate, &CollectionRecord::id)->sample_count == 2);
        EditCommand to_smart = add;
        to_smart.operation = new_uuid();
        to_smart.add_to_collection = long_collection->id;
        CHECK(library.apply_edit(to_smart).error().code == ErrorCode::InvalidArgument);
        TaxonomyCommand rename_missing{
            .kind = TaxonomyCommand::Kind::RenameCollection, .target_id = 999'999, .new_name = "Nope"};
        CHECK(library.apply_taxonomy(rename_missing).error().code == ErrorCode::NotFound);
    }

    TEST_CASE("BPM and key follow user value → user unknown → container → analysis → filename") {
        ScratchDirectory scratch;
        auto file = test::synthetic_file("Loop.wav", 10, 1'000'000);
        file.metadata->filename_bpm_milli = 128'000;
        auto populated = test::make_library(scratch / "lib", {file});
        auto& library = *populated.library;
        const auto loop = test::sample_id_named(library, "Loop");
        CHECK(library.sample_detail(loop)->bpm_milli == 128'000);
        CHECK(library.sample_detail(loop)->bpm_source == "filename");

        Library::AnalysisFacts facts{.sample_id = loop,
                                     .content_revision = 1,
                                     .bpm_milli = 127'500,
                                     .bpm_confidence = 0.9,
                                     .tempo_algorithm = "test",
                                     .tempo_requested = true};
        REQUIRE(library.record_analysis(facts).has_value());
        CHECK(library.sample_detail(loop)->bpm_milli == 127'500);

        EditCommand unknown;
        unknown.sample_ids = {loop};
        unknown.bpm = BpmOverrideEdit{OverrideState::Unknown, std::nullopt};
        REQUIRE(library.apply_edit(unknown).has_value());
        CHECK_FALSE(library.sample_detail(loop)->bpm_milli.has_value());

        EditCommand pinned;
        pinned.sample_ids = {loop};
        pinned.bpm = BpmOverrideEdit{OverrideState::Value, 130'000};
        REQUIRE(library.apply_edit(pinned).has_value());
        REQUIRE(library.record_analysis(facts).has_value());  // re-analysis never overrides the user
        CHECK(library.sample_detail(loop)->bpm_milli == 130'000);
        CHECK(library.sample_detail(loop)->bpm_source == "user");

        Library::AnalysisFacts out_of_range = facts;
        out_of_range.bpm_milli = 5'000;
        CHECK(library.record_analysis(out_of_range).error().code == ErrorCode::InvalidArgument);
        out_of_range = facts;
        out_of_range.byte_sha256 = "not hex";
        CHECK(library.record_analysis(out_of_range).error().code == ErrorCode::InvalidArgument);
    }

    // ------------------------------------------------------------ roots and scans

    TEST_CASE("finish_scan marks unseen files missing, except inside denied subtrees") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", {test::synthetic_file("a_b/one.wav", 10, 1'000),
                                                              test::synthetic_file("axb/two.wav", 20, 1'000),
                                                              test::synthetic_file("keep/three.wav", 30, 1'000)});
        auto& library = *populated.library;
        auto session = library.begin_scan(populated.root_id);
        REQUIRE(session.has_value());
        REQUIRE(library.record_scan_gap(*session, "a_b", /*denied=*/true).has_value());
        const std::vector<ScannedFile> seen = {test::synthetic_file("keep/three.wav", 30, 1'000)};
        REQUIRE(library.commit_scan_batch(*session, seen, {}, [](std::string_view) { return false; }).has_value());
        auto finished = library.finish_scan(*session, false);
        REQUIRE(finished.has_value());
        CHECK_FALSE(finished->complete_coverage);
        CHECK(finished->marked_missing == 1);
        CHECK(location_state(library, test::sample_id_named(library, "one")) == LocationState::Present);
        CHECK(location_state(library, test::sample_id_named(library, "two")) ==
              LocationState::Missing);  // '_' is not a wildcard
        CHECK(location_state(library, test::sample_id_named(library, "three")) == LocationState::Present);
    }

    TEST_CASE("scan batches reject oversized input and count unsafe paths") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        auto session = library.begin_scan(populated.root_id);
        REQUIRE(session.has_value());
        std::vector<ScannedFile> too_many(Library::kMaximumScanBatchFiles + 1, test::synthetic_file("x.wav", 1, 1'000));
        auto oversized = library.commit_scan_batch(*session, too_many, {}, [](std::string_view) { return false; });
        REQUIRE_FALSE(oversized.has_value());
        CHECK(oversized.error().code == ErrorCode::InvalidArgument);
        const std::vector<ScannedFile> unsafe = {test::synthetic_file("../escape.wav", 1, 1'000),
                                                 test::synthetic_file("bad\xff.wav", 1, 1'000)};
        auto outcome = library.commit_scan_batch(*session, unsafe, {}, [](std::string_view) { return false; });
        REQUIRE(outcome.has_value());
        CHECK(outcome->rejected_paths == 2);
        CHECK(outcome->added == 0);
    }

    TEST_CASE("case-sensitive roots keep files that differ only by case apart") {
        CHECK(mso::path_key("Drums/Kick.WAV", false) == mso::path_key("drums/kick.wav", false));
        CHECK(mso::path_key("Drums/Kick.WAV", true) == "Drums/Kick.WAV");
        CHECK(mso::path_key("Kick.wav", true) != mso::path_key("kick.wav", true));

        ScratchDirectory scratch;
        auto library = Library::create(scratch / "lib", "Cases");
        REQUIRE(library.has_value());
        const std::vector<ScannedFile> files = {test::synthetic_file("Kick.wav", 10, 1'000),
                                                test::synthetic_file("kick.wav", 20, 1'000)};
        const auto sensitive = add_scanned_root(**library, "Linux volume", files, true);
        const auto insensitive = add_scanned_root(**library, "Windows volume", files, false);
        auto roots = (*library)->list_roots();
        REQUIRE(roots.has_value());
        CHECK(std::ranges::find(*roots, sensitive, &RootRecord::id)->sample_count == 2);
        CHECK(std::ranges::find(*roots, sensitive, &RootRecord::id)->case_sensitive);
        CHECK(std::ranges::find(*roots, insensitive, &RootRecord::id)->sample_count == 1);
        auto stats = (*library)->known_stats_in_directory(sensitive, "");
        REQUIRE(stats.has_value());
        CHECK(stats->contains("Kick.wav"));
        CHECK(stats->contains("kick.wav"));
    }

    TEST_CASE("case sensitivity of a folder is detected without writing to it") {
        ScratchDirectory scratch;
        test::write_file(scratch / "Probe.txt", "x");
        auto detected = detect_case_sensitivity(scratch.path());
        REQUIRE(detected.has_value());
        const bool variant_is_same_file = std::filesystem::exists(scratch / "pROBE.txt");
        CHECK(*detected == !variant_is_same_file);
        std::size_t entries = 0;
        for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(scratch.path()))
            ++entries;
        CHECK(entries == 1);
        CHECK(detect_case_sensitivity(scratch / "missing").error().code == ErrorCode::NotFound);
    }

    TEST_CASE("renaming a root refreshes its samples' search paths") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        const auto before = current_revision(library);
        REQUIRE(library.rename_root(populated.root_id, "Renamed").has_value());
        CHECK(library.list_roots()->front().name == "Renamed");
        CHECK(query_text(library, std::format("SELECT path FROM sample_search WHERE sample_id={}", kick)) ==
              "Renamed/Drums/Kick 01.wav");
        auto changes = library.changes_since(before);
        REQUIRE(changes.has_value());
        CHECK(changes->sample_ids.size() == 3);
        CHECK(changes->structure_changed);
        CHECK(library.rename_root(999'999, "Nope").error().code == ErrorCode::NotFound);
        CHECK(library.rename_root(populated.root_id, "  ").error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("root availability commits only real changes") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto before = current_revision(library);
        REQUIRE(library.set_root_availability(populated.root_id, Availability::Online).has_value());  // no-op
        CHECK(current_revision(library) == before);
        REQUIRE(library.set_root_availability(populated.root_id, Availability::Offline).has_value());
        CHECK(current_revision(library) == before + 1);
        CHECK(library.list_roots()->front().availability == Availability::Offline);
        auto changes = library.changes_since(before);
        REQUIRE(changes.has_value());
        CHECK(changes->sample_ids == all_sample_ids(library));
        CHECK(changes->structure_changed);
        CHECK(library.set_root_availability(999'999, Availability::Online).error().code == ErrorCode::NotFound);
    }

    // ------------------------------------------------------------ change feed

    TEST_CASE("changes_since reports changed samples and structure, and acknowledging prunes the outbox") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        const auto snare = test::sample_id_named(library, "Snare");
        const auto start = current_revision(library);

        CHECK(library.changes_since(start)->sample_ids.empty());
        REQUIRE(library.apply_edit(rate({kick, snare}, 3)).has_value());
        auto rated = library.changes_since(start);
        REQUIRE(rated.has_value());
        CHECK_FALSE(rated->full_reload);
        CHECK(rated->revision == start + 1);
        CHECK(rated->sample_ids == std::vector<std::int64_t>{std::min(kick, snare), std::max(kick, snare)});
        CHECK_FALSE(rated->structure_changed);

        EditCommand tag;
        tag.sample_ids = {kick};
        tag.tags_add = {"Drums"};
        REQUIRE(library.apply_edit(tag).has_value());
        auto tagged = library.changes_since(start + 1);
        REQUIRE(tagged.has_value());
        CHECK(tagged->sample_ids == std::vector<std::int64_t>{kick});
        CHECK(tagged->structure_changed);

        REQUIRE(library.undo().has_value());
        auto undone = library.changes_since(start + 2);
        REQUIRE(undone.has_value());
        CHECK(undone->sample_ids == std::vector<std::int64_t>{kick});

        const auto latest = current_revision(library);
        CHECK(library.changes_since(latest + 5)->full_reload);  // a consumer ahead of the catalog
        REQUIRE(library.apply_edit(rate({kick}, 1)).has_value());
        REQUIRE(library.apply_edit(rate({kick}, 2)).has_value());
        CHECK(library.changes_since(latest, 1)->full_reload);  // more envelopes than the limit
        CHECK_FALSE(library.changes_since(latest, 2)->full_reload);

        REQUIRE(library.acknowledge_changes("ui", latest).has_value());
        CHECK(query(library, std::format("SELECT count(*) FROM catalog_outbox WHERE revision<={}", latest)) == 0);
        auto pruned = library.changes_since(start);
        REQUIRE(pruned.has_value());
        CHECK(pruned->full_reload);
        CHECK(pruned->revision == current_revision(library));
        CHECK_FALSE(library.changes_since(latest)->full_reload);

        // The slowest consumer holds the outbox.
        REQUIRE(library.acknowledge_changes("export", latest).has_value());
        REQUIRE(library.acknowledge_changes("ui", current_revision(library)).has_value());
        CHECK(query(library, std::format("SELECT count(*) FROM catalog_outbox WHERE revision>{}", latest)) == 2);
        CHECK(library.acknowledge_changes("", 0).error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("every mutation's envelope carries samples and structure") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        REQUIRE(library.ensure_tag("Envelope").has_value());
        REQUIRE(library.set_root_availability(populated.root_id, Availability::Offline).has_value());
        REQUIRE(library.create_analysis_run_for_root(populated.root_id, "Tempo", {"tempo"}, false).has_value());
        auto reader = library.acquire_reader();
        REQUIRE(reader.has_value());
        auto statement = reader->connection().prepare("SELECT envelope_version, payload_json FROM catalog_outbox");
        REQUIRE(statement.has_value());
        int envelopes = 0;
        while (true) {
            auto has_row = statement->next_row();
            REQUIRE(has_row.has_value());
            if (!*has_row) break;
            ++envelopes;
            CHECK(statement->column_int64(0) == 2);
            const auto payload = nlohmann::json::parse(statement->column_text(1));
            CHECK(payload.contains("samples"));
            CHECK(payload.contains("structure"));
        }
        CHECK(envelopes >= 5);
    }

    // ------------------------------------------------------------ analysis queue

    TEST_CASE("analysis jobs are fenced: cancelled runs and changed content never publish results") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        REQUIRE(library.set_root_availability(populated.root_id, Availability::Online).has_value());

        auto run = library.create_analysis_run({"Musical", {"tempo", "key"}, {kick, kick}, false});
        REQUIRE(run.has_value());
        CHECK(run->queued == 1);  // duplicate ids are one job
        auto lease = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(lease.has_value());
        REQUIRE(lease->has_value());
        CHECK((*lease)->stages == std::vector<std::string>{"tempo", "key"});
        REQUIRE(library.control_run(run->id, Library::RunControl::Cancel).has_value());
        Library::AnalysisFacts late{.sample_id = kick,
                                    .content_revision = (*lease)->content_revision,
                                    .bpm_milli = 100'000,
                                    .tempo_algorithm = "test",
                                    .tempo_requested = true};
        auto published = library.complete_job(**lease, late);
        REQUIRE_FALSE(published.has_value());
        CHECK(published.error().code == ErrorCode::Cancelled);
        CHECK_FALSE(library.sample_detail(kick)->bpm_milli.has_value());
        CHECK(library.control_run(999'999, Library::RunControl::Pause).error().code == ErrorCode::NotFound);

        auto second = library.create_analysis_run({"Musical", {"tempo"}, {kick}, false});
        REQUIRE(second.has_value());
        auto second_lease = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(second_lease.has_value());
        REQUIRE(second_lease->has_value());
        execute(library, std::format("UPDATE samples SET content_revision=content_revision+1 WHERE id={}", kick));
        auto stale = library.complete_job(**second_lease, late);
        REQUIRE_FALSE(stale.has_value());
        CHECK(stale.error().code == ErrorCode::InputChanged);
        auto runs = library.list_analysis_runs();
        REQUIRE(runs.has_value());
        CHECK(runs->front().state == "completed");
        CHECK(runs->front().stale == 1);
    }

    TEST_CASE("results must match their lease, and out-of-range results fail the job") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        const auto snare = test::sample_id_named(library, "Snare");
        REQUIRE(library.create_analysis_run({"Tempo", {"tempo"}, {kick, snare}, false}).has_value());

        auto first = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(first.has_value());
        REQUIRE(first->has_value());
        Library::AnalysisFacts wrong_sample{.sample_id = (*first)->sample_id == kick ? snare : kick,
                                            .content_revision = (*first)->content_revision,
                                            .bpm_milli = 120'000,
                                            .tempo_algorithm = "test",
                                            .tempo_requested = true};
        CHECK(library.complete_job(**first, wrong_sample).error().code == ErrorCode::InputChanged);
        CHECK(job_state(library, (*first)->job_id) == "stale");

        auto second = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(second.has_value());
        REQUIRE(second->has_value());
        Library::AnalysisFacts bad{.sample_id = (*second)->sample_id,
                                   .content_revision = (*second)->content_revision,
                                   .bpm_milli = 1,
                                   .tempo_algorithm = "test",
                                   .tempo_requested = true};
        CHECK(library.complete_job(**second, bad).error().code == ErrorCode::InvalidArgument);
        CHECK(job_state(library, (*second)->job_id) == "failed");
        CHECK(library.list_analysis_runs()->front().state == "completed_with_errors");
    }

    TEST_CASE("analysis skips samples whose results are current unless forced") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        Library::AnalysisFacts facts{
            .sample_id = kick, .content_revision = 1, .tempo_algorithm = "test", .tempo_requested = true};
        REQUIRE(library.record_analysis(facts).has_value());
        auto run = library.create_analysis_run({"Tempo", {"tempo"}, {kick}, false});
        REQUIRE(run.has_value());
        CHECK(run->queued == 0);
        CHECK(run->skipped == 1);
        CHECK(run->state == "completed");
        auto forced = library.create_analysis_run({"Tempo", {"tempo"}, {kick}, true});
        REQUIRE(forced.has_value());
        CHECK(forced->queued == 1);
        CHECK(library.create_analysis_run({"Tempo", {"telepathy"}, {kick}, true}).error().code ==
              ErrorCode::InvalidArgument);
    }

    TEST_CASE("a run for a whole root is created set-based and skips current results") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        Library::AnalysisFacts facts{
            .sample_id = kick, .content_revision = 1, .tempo_algorithm = "test", .tempo_requested = true};
        REQUIRE(library.record_analysis(facts).has_value());

        auto run = library.create_analysis_run_for_root(populated.root_id, "Automatic", {"tempo"}, false);
        REQUIRE(run.has_value());
        CHECK(run->queued == 2);
        CHECK(run->skipped == 1);
        auto listed = library.list_analysis_runs();
        REQUIRE(listed.has_value());
        CHECK(listed->front().queued == 2);
        CHECK(listed->front().skipped == 1);
        auto forced = library.create_analysis_run_for_root(populated.root_id, "Automatic", {"tempo"}, true);
        REQUIRE(forced.has_value());
        CHECK(forced->queued == 3);
        CHECK(library.create_analysis_run_for_root(999'999, "Automatic", {"tempo"}, false).error().code ==
              ErrorCode::NotFound);

        REQUIRE(library.set_root_availability(populated.root_id, Availability::Offline).has_value());
        auto offline = library.create_analysis_run_for_root(populated.root_id, "Automatic", {"key"}, false);
        REQUIRE(offline.has_value());
        CHECK(offline->queued == 0);
        CHECK(offline->blocked == 3);
    }

    TEST_CASE("an offline root never starves the analysis queue; its jobs resume when it is back") {
        ScratchDirectory scratch;
        std::vector<ScannedFile> offline_files;
        for (int index = 0; index < 40; ++index)
            offline_files.push_back(test::synthetic_file(std::format("far/{:02}.wav", index), 10 + index, 1'000));
        auto populated = test::make_library(scratch / "lib", offline_files);
        auto& library = *populated.library;
        const auto near_root = add_scanned_root(library, "Near", {test::synthetic_file("near.wav", 5, 1'000)});
        const auto near_sample = test::sample_id_named(library, "near");

        REQUIRE(library.create_analysis_run_for_root(populated.root_id, "Far", {"tempo"}, false).has_value());
        REQUIRE(library.create_analysis_run_for_root(near_root, "Near", {"tempo"}, false).has_value());
        REQUIRE(library.set_root_availability(populated.root_id, Availability::Offline).has_value());
        // Even jobs that were never marked blocked are skipped by the reachability filter.
        execute(library, "UPDATE jobs SET state='queued', error_code=NULL WHERE state='blocked'");

        auto lease = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(lease.has_value());
        REQUIRE(lease->has_value());
        CHECK((*lease)->sample_id == near_sample);
        REQUIRE(library.release_job(**lease).has_value());

        REQUIRE(library.set_root_availability(populated.root_id, Availability::Offline).has_value());  // no-op
        REQUIRE(library.set_root_availability(populated.root_id, Availability::Online).has_value());
        CHECK(query(library, "SELECT count(*) FROM jobs WHERE state='blocked'") == 0);
        auto far_lease = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(far_lease.has_value());
        REQUIRE(far_lease->has_value());
        CHECK((*far_lease)->sample_id != near_sample);  // the older run comes first again
    }

    TEST_CASE("expired leases are reclaimed, and a job that keeps expiring fails") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        REQUIRE(library.create_analysis_run({"Tempo", {"tempo"}, {kick}, false}).has_value());
        std::int64_t job_id = 0;
        for (int attempt = 1; attempt <= Library::kMaximumJobAttempts; ++attempt) {
            std::optional<Library::JobLease> lease;
            REQUIRE(test::wait_until(
                [&] {
                    auto next = library.lease_next_job(std::chrono::milliseconds(1));
                    if (next && *next) lease = **next;
                    return lease.has_value();
                },
                std::chrono::seconds(5)));
            CHECK(lease->attempt == attempt);
            job_id = lease->job_id;
        }
        // The last lease expires too: the job fails instead of being retried forever.
        REQUIRE(test::wait_until(
            [&] {
                auto next = library.lease_next_job(std::chrono::milliseconds(1));
                return next.has_value() && !next->has_value() && job_state(library, job_id) == "failed";
            },
            std::chrono::seconds(5)));
        CHECK(library.list_analysis_runs()->front().state == "completed_with_errors");
    }

    TEST_CASE("transient failures back off, and renewals need the lease") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        REQUIRE(library.create_analysis_run({"Tempo", {"tempo"}, {kick}, false}).has_value());
        auto lease = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(lease.has_value());
        REQUIRE(lease->has_value());
        CHECK(library.renew_lease(**lease, std::chrono::seconds(30)).has_value());

        const auto before = now_unix_ms();
        REQUIRE(library.fail_job(**lease, ErrorCode::CorruptAudio, /*transient=*/true).has_value());
        CHECK(job_state(library, (*lease)->job_id) == "queued");
        const auto not_before =
            query(library, std::format("SELECT lease_expires_at FROM jobs WHERE id={}", (*lease)->job_id));
        CHECK(not_before >= before + 1'000);
        auto too_early = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(too_early.has_value());
        CHECK_FALSE(too_early->has_value());  // still backing off

        auto renewed = library.renew_lease(**lease, std::chrono::seconds(30));
        REQUIRE_FALSE(renewed.has_value());
        CHECK(renewed.error().code == ErrorCode::Cancelled);
        CHECK(library.fail_job(**lease, ErrorCode::CorruptAudio, true).has_value());  // lost lease: ignored
        CHECK(job_state(library, (*lease)->job_id) == "queued");
    }

    TEST_CASE("a job blocked by a missing file waits for a scan that finds it") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        REQUIRE(library.create_analysis_run({"Tempo", {"tempo"}, {kick}, false}).has_value());
        auto lease = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(lease.has_value());
        REQUIRE(lease->has_value());
        REQUIRE(library.fail_job(**lease, ErrorCode::FileMissing, false).has_value());
        CHECK(job_state(library, (*lease)->job_id) == "blocked");

        auto session = library.begin_scan(populated.root_id);
        REQUIRE(session.has_value());
        REQUIRE(library.commit_scan_batch(*session, three_samples(), {}, [](std::string_view) { return false; })
                    .has_value());
        REQUIRE(library.finish_scan(*session, false).has_value());
        CHECK(job_state(library, (*lease)->job_id) == "queued");
    }

}  // TEST_SUITE("catalog")
