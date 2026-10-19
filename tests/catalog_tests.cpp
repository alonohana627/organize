// Catalog: identity, migrations, edits, durable undo/redo, conflicts, idempotency, taxonomy, scans,
// root availability, the change feed, crash recovery, backups and the analysis queue.
#include <doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <regex>
#include <set>
#include <stop_token>

#include <nlohmann/json.hpp>

#include "mso/catalog/detail/catalog_internal.hpp"
#include "mso/catalog/durable_file.hpp"
#include "mso/catalog/library.hpp"
#include "mso/catalog/migrations.hpp"
#include "mso/catalog/root_paths.hpp"
#include "test_support.hpp"

using namespace mso;
using namespace mso::catalog;
using mso::test::ScratchDirectory;

namespace mso::catalog {

/// Friend of Library (declared in library.hpp, defined only here) giving these tests the private
/// writer connection for fault injection: simulating a concurrent tool, corrupt rows or an injected
/// failure. Production code has no way to reach it.
struct LibraryTestAccess {
    /// The serialized writer connection (hold writer_mutex while using it from several threads).
    static sqlite::Connection& writer(Library& library) { return library.writer_; }
};

}  // namespace mso::catalog

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

/// Writes a catalog with only the migrations up to `version` (as released before this build) plus its
/// manifest.
void write_library_at_version(const std::filesystem::path& directory, const uuids::uuid& uuid, int version) {
    std::filesystem::create_directories(directory);
    auto connection = sqlite::Connection::open(directory / Library::kCatalogFileName, {.create = true});
    REQUIRE(connection.has_value());
    for (const auto& migration : migrations()) {
        if (migration.version > version) break;
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
    REQUIRE(connection
                ->execute_script(
                    std::format("UPDATE library_state SET schema_version={0}; PRAGMA user_version={0}", version))
                .has_value());
    connection->close();
    const nlohmann::json manifest = {{"format_version", 1}, {"library_uuid", uuids::to_string(uuid)}, {"name", "Old"}};
    REQUIRE(write_file_atomically(directory / Library::kManifestFileName, manifest.dump()).has_value());
}

/// Every compiled statement idle in the library's statement caches: the writer's and each reader's.
/// Readers are borrowed all at once so every pooled connection is visited.
std::set<std::string> cached_statements(Library& library) {
    std::set<std::string> statements;
    for (auto& sql : writer(library).cached_statement_sql())
        statements.insert(std::move(sql));
    std::vector<ReaderLease> readers;
    for (int reader = 0; reader < OpenOptions{}.max_readers; ++reader) {
        auto lease = library.acquire_reader();
        REQUIRE(lease.has_value());
        readers.push_back(std::move(*lease));
    }
    for (auto& reader : readers)
        for (auto& sql : reader.connection().cached_statement_sql())
            statements.insert(std::move(sql));
    return statements;
}

/// The names a statement uses for `locations` and `jobs`: the table names and every alias it gives
/// them ("locations location", "jobs AS job").
std::set<std::string> covered_table_names(const std::string& sql) {
    static const std::set<std::string> kNotAliases = {
        "WHERE",  "SET",     "ON",        "JOIN",   "LEFT",   "INNER",     "CROSS",  "INDEXED", "NOT",   "GROUP",
        "ORDER",  "LIMIT",   "USING",     "WHEN",   "AND",    "OR",        "UNION",  "EXCEPT",  "AS",    "VALUES",
        "SELECT", "DEFAULT", "RETURNING", "HAVING", "WINDOW", "INTERSECT", "UPDATE", "DELETE",  "INSERT"};
    static const std::regex kTable(R"(\b(locations|jobs)\b(?:\s+(?:AS\s+)?([A-Za-z_][A-Za-z_0-9]*))?)",
                                   std::regex::icase);
    std::set<std::string> names;
    for (std::sregex_iterator match(sql.begin(), sql.end(), kTable), end; match != end; ++match) {
        names.insert((*match)[1].str());
        std::string alias = (*match)[2].str();
        std::string upper = alias;
        std::ranges::transform(upper, upper.begin(), [](unsigned char letter) { return std::toupper(letter); });
        if (!alias.empty() && !kNotAliases.contains(upper)) names.insert(std::move(alias));
    }
    return names;
}

/// The EXPLAIN QUERY PLAN lines of `sql` that read a whole `locations` or `jobs` table or index
/// ("SCAN <name>", with or without an index) or sort in a temporary B-tree.
std::vector<std::string> full_scans_and_sorts(sqlite::Connection& connection, const std::string& sql) {
    const auto names = covered_table_names(sql);
    auto plan = connection.prepare("EXPLAIN QUERY PLAN " + sql);
    REQUIRE_MESSAGE(plan.has_value(), plan.error().message(), " in: ", sql);
    std::vector<std::string> findings;
    while (true) {
        auto has_row = plan->next_row();
        REQUIRE(has_row.has_value());
        if (!*has_row) break;
        const std::string detail = plan->column_text(3);
        static const std::regex kScan(R"(^SCAN (\S+))");
        std::smatch scanned;
        if ((std::regex_search(detail, scanned, kScan) && names.contains(scanned[1].str())) ||
            detail.starts_with("USE TEMP B-TREE"))
            findings.push_back(detail);
    }
    return findings;
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
            {6, "d626e8ddfd71f63dfdc0c742c4942cfdf2dd2f85fca222dc844667ec36817b5c"},
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
        write_library_at_version(scratch / "lib", uuid, 4);
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

    TEST_CASE("a chunked edit whose committed chunks changed nothing resumes after them, with no action yet") {
        ScratchDirectory scratch;
        std::vector<ScannedFile> files;
        for (int index = 0; index < 1100; ++index)
            files.push_back(test::synthetic_file(std::format("bulk/{:04}.wav", index), 100 + index, 1'000'000));
        auto populated = test::make_library(scratch / "lib", files);
        auto& library = *populated.library;
        EditCommand edit;
        edit.sample_ids = all_sample_ids(library);
        edit.favorite = true;
        // The whole first chunk is already favorite (a no-op), and the second chunk fails.
        execute(library, std::format("UPDATE samples SET favorite=1 WHERE id<={}",
                                     edit.sample_ids[EditCommand::kSamplesPerChunk - 1]));
        execute(library, std::format("CREATE TEMP TRIGGER inject_failure BEFORE UPDATE OF favorite ON samples "
                                     "WHEN new.id={} BEGIN SELECT RAISE(ABORT, 'injected failure'); END",
                                     edit.sample_ids[700]));
        REQUIRE_FALSE(library.apply_edit(edit).has_value());
        CHECK(library.history()->empty());
        CHECK(query_text(library, std::format("SELECT state FROM command_receipts WHERE operation_uuid='{}'",
                                              uuids::to_string(edit.operation))) == "partial");

        execute(library, "DROP TRIGGER temp.inject_failure");
        auto resumed = library.apply_edit(edit);
        REQUIRE_MESSAGE(resumed.has_value(), resumed.error().message());
        CHECK_FALSE(resumed->replayed);
        CHECK_FALSE(resumed->partial);
        CHECK(resumed->applied == 1100);  // samples processed, as for an edit that never failed
        CHECK(resumed->history_action.has_value());
        CHECK(query(library, "SELECT count(*) FROM samples WHERE favorite=1") == 1100);
        auto history = library.history();
        REQUIRE(history.has_value());
        REQUIRE(history->size() == 1);
        CHECK(history->front().state == "applied");
        REQUIRE(library.undo().has_value());  // undoes exactly what this edit changed
        CHECK(query(library, "SELECT count(*) FROM samples WHERE favorite=1") == 500);
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

    // ------------------------------------------------------------ second review round

    TEST_CASE("skipped entries stay seen: never missing, and a missing one comes back as unknown") {
        ScratchDirectory scratch;
        auto library = Library::create(scratch / "lib", "Skips");
        REQUIRE(library.has_value());
        auto& catalog = **library;
        const std::vector<ScannedFile> files = {
            test::synthetic_file("Cloud/Pad.wav", 10, 1'000), test::synthetic_file("Links/Loop.wav", 20, 1'000),
            test::synthetic_file("Drums/Kick.wav", 30, 1'000), test::synthetic_file("Gone/Old.wav", 40, 1'000)};
        const auto root = add_scanned_root(catalog, "Root", files, /*case_sensitive=*/false);

        auto session = catalog.begin_scan(root);
        REQUIRE(session.has_value());
        const std::vector<ScannedFile> probed = {test::synthetic_file("Drums/Kick.wav", 30, 1'000)};
        REQUIRE(catalog.commit_scan_batch(*session, probed, {}, [](std::string_view) { return false; }).has_value());
        const auto before = current_revision(catalog);
        // A cloud placeholder, a link spelled in another case, and a path the catalog never saw.
        const std::vector<std::string> skipped = {"Cloud/Pad.wav", "LINKS/loop.WAV", "Unknown/new.wav"};
        REQUIRE(catalog.mark_scan_entries_seen(*session, skipped).has_value());
        CHECK(current_revision(catalog) == before);  // bookkeeping only: nothing visible changed
        auto finished = catalog.finish_scan(*session, false);
        REQUIRE(finished.has_value());
        CHECK(finished->marked_missing == 1);
        CHECK(location_state(catalog, test::sample_id_named(catalog, "Pad")) == LocationState::Present);
        CHECK(location_state(catalog, test::sample_id_named(catalog, "Loop")) == LocationState::Present);
        CHECK(location_state(catalog, test::sample_id_named(catalog, "Kick")) == LocationState::Present);
        CHECK(location_state(catalog, test::sample_id_named(catalog, "Old")) == LocationState::Missing);
        CHECK(query(catalog, "SELECT byte_size FROM locations WHERE relative_path='Cloud/Pad.wav'") == 10);

        // The missing file is back but skipped (say it became a cloud placeholder): it exists, but its
        // bytes were not examined, so it is unknown rather than present; the change is a revision.
        auto again = catalog.begin_scan(root);
        REQUIRE(again.has_value());
        const std::vector<std::string> back = {"Gone/Old.wav"};
        REQUIRE(catalog.mark_scan_entries_seen(*again, back).has_value());
        CHECK(current_revision(catalog) == before + 2);
        CHECK(location_state(catalog, test::sample_id_named(catalog, "Old")) == LocationState::Unknown);

        const std::vector<std::string> too_many(Library::kMaximumScanBatchFiles + 1, "Cloud/Pad.wav");
        auto oversized = catalog.mark_scan_entries_seen(*again, too_many);
        REQUIRE_FALSE(oversized.has_value());
        CHECK(oversized.error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("case-insensitive roots: a case-only rename is stored, and a gap in another case protects its files") {
        ScratchDirectory scratch;
        auto library = Library::create(scratch / "lib", "Cases");
        REQUIRE(library.has_value());
        auto& catalog = **library;
        const std::vector<ScannedFile> files = {test::synthetic_file("Drums/Kick.wav", 10, 1'000),
                                                test::synthetic_file("Loops/Beat.wav", 20, 1'000)};
        const auto root = add_scanned_root(catalog, "Windows volume", files, /*case_sensitive=*/false);
        const auto kick = test::sample_id_named(catalog, "Kick");

        auto session = catalog.begin_scan(root);
        REQUIRE(session.has_value());
        const std::vector<ScannedFile> renamed = {test::synthetic_file("drums/KICK.wav", 10, 1'000)};
        auto committed = catalog.commit_scan_batch(*session, renamed, {}, [](std::string_view) { return false; });
        REQUIRE(committed.has_value());
        CHECK(committed->unchanged == 1);
        REQUIRE(catalog.record_scan_gap(*session, "LOOPS", /*denied=*/true).has_value());
        auto finished = catalog.finish_scan(*session, false);
        REQUIRE(finished.has_value());
        CHECK(finished->marked_missing == 0);  // Loops/Beat.wav is inside the denied subtree
        auto detail = catalog.sample_detail(kick);
        REQUIRE(detail.has_value());
        REQUIRE(detail->locations.size() == 1);
        CHECK(detail->locations.front().relative_path == "drums/KICK.wav");
        CHECK(location_state(catalog, test::sample_id_named(catalog, "Beat")) == LocationState::Present);
        auto folders = catalog.list_folders(root);
        REQUIRE(folders.has_value());
        CHECK(std::ranges::count(*folders, std::string("drums"), &FolderRecord::relative_path) == 1);
    }

    TEST_CASE("deep folder trees are stored without recursion, and emptied folders are removed") {
        ScratchDirectory scratch;
        std::string deep;
        for (int level = 0; level < 150; ++level)
            deep += std::format("level{}/", level);
        auto moved = test::synthetic_file("Old/Moved.wav", 10, 1'000);
        moved.volume_id = "volume";
        moved.file_id = std::string(16, 'x');
        auto populated =
            test::make_library(scratch / "lib", {test::synthetic_file(deep + "Deep.wav", 5, 1'000), moved});
        auto& library = *populated.library;
        auto folders = library.list_folders(populated.root_id);
        REQUIRE(folders.has_value());
        CHECK(folders->size() == 151);

        // A same-volume rename moves the file out of Old/, which then holds nothing and goes away.
        auto session = library.begin_scan(populated.root_id);
        REQUIRE(session.has_value());
        auto renamed = moved;
        renamed.relative_path = "New/Moved.wav";
        const std::vector<ScannedFile> files = {test::synthetic_file(deep + "Deep.wav", 5, 1'000), renamed};
        auto committed = library.commit_scan_batch(*session, files, {}, [](std::string_view) { return false; });
        REQUIRE(committed.has_value());
        CHECK(committed->relinked == 1);
        auto finished = library.finish_scan(*session, false);
        REQUIRE(finished.has_value());
        CHECK(finished->removed_empty_folders == 1);
        folders = library.list_folders(populated.root_id);
        REQUIRE(folders.has_value());
        CHECK(std::ranges::count(*folders, std::string("Old"), &FolderRecord::relative_path) == 0);
        CHECK(std::ranges::count(*folders, std::string("New"), &FolderRecord::relative_path) == 1);
        CHECK(folders->size() == 151);
    }

    TEST_CASE("begin_scan resumes a recent interrupted scan but abandons one that is too old") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        auto first = library.begin_scan(populated.root_id);
        REQUIRE(first.has_value());
        REQUIRE(library.finish_scan(*first, /*cancelled=*/true).has_value());
        auto resumed = library.begin_scan(populated.root_id);
        REQUIRE(resumed.has_value());
        CHECK(resumed->id == first->id);
        REQUIRE(library.finish_scan(*resumed, /*cancelled=*/true).has_value());

        execute(library, std::format("UPDATE scan_sessions SET started_at=0 WHERE id={}", first->id));
        auto fresh = library.begin_scan(populated.root_id);
        REQUIRE(fresh.has_value());
        CHECK(fresh->id != first->id);
        CHECK(fresh->generation > first->generation);
        CHECK(query_text(library, std::format("SELECT state FROM scan_sessions WHERE id={}", first->id)) ==
              "cancelled");
    }

    TEST_CASE("many scan gaps are recorded in one call and protect their subtrees; known stats carry file identity") {
        ScratchDirectory scratch;
        auto library = Library::create(scratch / "lib", "Gaps");
        REQUIRE(library.has_value());
        auto& catalog = **library;
        auto identified = test::synthetic_file("Open/Kick.wav", 10, 1'000);
        identified.volume_id = "0011aabb";
        identified.file_id = std::string("\x01\x00\x02\xff", 4) + std::string(12, 'i');
        const std::vector<ScannedFile> files = {identified, test::synthetic_file("Denied/Pad.wav", 20, 1'000),
                                                test::synthetic_file("Failed/Loop.wav", 30, 1'000)};
        const auto root = add_scanned_root(catalog, "Root", files, /*case_sensitive=*/false);

        auto stats = catalog.known_stats_in_directory(root, "Open");
        REQUIRE(stats.has_value());
        const auto& known = stats->at(path_key("Open/Kick.wav", false));
        CHECK(known.byte_size == 10);
        CHECK(known.volume_id == identified.volume_id);
        CHECK(known.file_id == identified.file_id);  // raw bytes, NUL and 0xFF included
        auto unidentified = catalog.known_stats_in_directory(root, "Denied");
        REQUIRE(unidentified.has_value());
        CHECK_FALSE(unidentified->begin()->second.file_id.has_value());

        auto session = catalog.begin_scan(root);
        REQUIRE(session.has_value());
        REQUIRE(
            catalog.commit_scan_batch(*session, std::span(files).first(1), {}, [](std::string_view) { return false; })
                .has_value());
        const std::vector<ScanGap> gaps = {{.relative_directory = "DENIED", .denied = true},
                                           {.relative_directory = "Failed", .denied = false}};
        REQUIRE(catalog.record_scan_gaps(*session, gaps).has_value());
        REQUIRE(catalog.record_scan_gaps(*session, {}).has_value());
        CHECK(query(catalog, "SELECT count(*) FROM scan_frontier WHERE state IN ('denied','failed')") == 2);
        const std::vector<ScanGap> too_many(Library::kMaximumScanBatchFiles + 1);
        CHECK(catalog.record_scan_gaps(*session, too_many).error().code == ErrorCode::InvalidArgument);
        auto finished = catalog.finish_scan(*session, false);
        REQUIRE(finished.has_value());
        CHECK_FALSE(finished->complete_coverage);
        CHECK(finished->marked_missing == 0);  // both files sit inside a gap
        CHECK(location_state(catalog, test::sample_id_named(catalog, "Pad")) == LocationState::Present);
        CHECK(location_state(catalog, test::sample_id_named(catalog, "Loop")) == LocationState::Present);
    }

    TEST_CASE("scan frontier rows live only while their scan can resume") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto frontier_rows = [&] { return query(library, "SELECT count(*) FROM scan_frontier"); };
        const auto none = [](std::string_view) { return false; };

        // A cancelled scan keeps its completed directories for the resumed run.
        auto interrupted = library.begin_scan(populated.root_id);
        REQUIRE(interrupted.has_value());
        REQUIRE(library.commit_scan_batch(*interrupted, three_samples(), std::vector<std::string>{"Drums"}, none)
                    .has_value());
        REQUIRE(library.record_scan_gap(*interrupted, "Denied", /*denied=*/true).has_value());
        REQUIRE(library.finish_scan(*interrupted, /*cancelled=*/true).has_value());
        CHECK(frontier_rows() == 2);
        auto resumed = library.begin_scan(populated.root_id);
        REQUIRE(resumed.has_value());
        REQUIRE(resumed->id == interrupted->id);
        CHECK(library.completed_directories(*resumed).value_or(std::vector<std::string>{}) ==
              std::vector<std::string>{"Drums"});
        // Completing it drops them.
        REQUIRE(library.finish_scan(*resumed, false).has_value());
        CHECK(frontier_rows() == 0);

        // Rows a finished session left behind (catalogs from before this rule) go at the next scan.
        execute(library, std::format("INSERT INTO scan_frontier(scan_id,relative_directory,state) "
                                     "VALUES({0},'Old','complete'),({0},'Older','complete')",
                                     resumed->id));
        auto next = library.begin_scan(populated.root_id);
        REQUIRE(next.has_value());
        CHECK(next->id != resumed->id);
        CHECK(frontier_rows() == 0);
    }

    TEST_CASE("a conflicting undo keeps its receipt: resending it reports the conflict again") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        const auto snare = test::sample_id_named(library, "Snare");
        REQUIRE(library.apply_edit(rate({kick}, 1)).has_value());
        REQUIRE(library.apply_edit(rate({snare}, 3)).has_value());
        execute(library, std::format("UPDATE samples SET rating=5 WHERE id={}", snare));
        const auto operation = new_uuid();
        auto first = library.undo(operation);
        REQUIRE_FALSE(first.has_value());
        CHECK(first.error().code == ErrorCode::HistoryConflict);
        auto resent = library.undo(operation);
        REQUIRE_FALSE(resent.has_value());
        CHECK(resent.error().code == ErrorCode::HistoryConflict);
        CHECK(library.sample_detail(kick)->rating == 1);  // the older action was not undone instead
    }

    TEST_CASE("a conflicting redo is recorded once and supersedes the rest of the redo tail") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        const auto snare = test::sample_id_named(library, "Snare");
        REQUIRE(library.apply_edit(rate({kick}, 1)).has_value());
        REQUIRE(library.apply_edit(rate({snare}, 3)).has_value());
        REQUIRE(library.undo().has_value());
        REQUIRE(library.undo().has_value());
        execute(library, std::format("UPDATE samples SET rating=4 WHERE id={}", kick));
        auto redone = library.redo();
        REQUIRE_FALSE(redone.has_value());
        CHECK(redone.error().code == ErrorCode::HistoryConflict);
        CHECK(library.sample_detail(kick)->rating == 4);
        auto history = library.history();
        REQUIRE(history.has_value());
        REQUIRE(history->size() == 1);  // the newer undone action was superseded
        CHECK(history->front().state == "conflict");
        auto again = library.redo();
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().code == ErrorCode::NotFound);
        auto review = library.open_review_items();
        REQUIRE(review.has_value());
        CHECK(std::ranges::count(*review, std::string("history_conflict"), &ReviewItem::kind) == 1);
    }

    TEST_CASE("an edit that changes nothing leaves history and the redo tail alone") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        REQUIRE(library.apply_edit(rate({kick}, 1)).has_value());
        REQUIRE(library.undo().has_value());
        auto unchanged = library.apply_edit(rate({kick}, 0));  // already 0
        REQUIRE(unchanged.has_value());
        CHECK_FALSE(unchanged->history_action.has_value());
        CHECK(unchanged->applied == 1);
        auto replayed = library.apply_edit([&] {
            auto command = rate({kick}, 0);
            command.operation = unchanged->operation;
            return command;
        }());
        REQUIRE(replayed.has_value());
        CHECK(replayed->replayed);
        REQUIRE(library.history()->size() == 1);
        REQUIRE(library.redo().has_value());
        CHECK(library.sample_detail(kick)->rating == 1);
    }

    TEST_CASE("the outbox is pruned to its byte cap, and lagging consumers are told to reload") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        for (int rating = 1; rating <= 5; ++rating)
            REQUIRE(library.apply_edit(rate({kick}, rating)).has_value());
        const auto newest = current_revision(library);
        const auto kept_bytes = query(library, std::format("SELECT sum(length(payload_json)) FROM catalog_outbox "
                                                           "WHERE revision>{}",
                                                           newest - 2));
        REQUIRE(detail::enforce_outbox_cap(writer(library), kept_bytes).has_value());
        CHECK(query(library, "SELECT count(*) FROM catalog_outbox") == 2);
        auto lagging = library.changes_since(0);
        REQUIRE(lagging.has_value());
        CHECK(lagging->full_reload);
        auto current = library.changes_since(newest - 2);
        REQUIRE(current.has_value());
        CHECK_FALSE(current->full_reload);
    }

    TEST_CASE("pausing a run takes its waiting jobs out of the queue, and a paused run still settles") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        auto run = library.create_analysis_run({"Tempo", {"tempo"}, all_sample_ids(library), false});
        REQUIRE(run.has_value());
        auto leased = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(leased.has_value());
        REQUIRE(leased->has_value());
        REQUIRE(library.control_run(run->id, Library::RunControl::Pause).has_value());
        auto runs = library.list_analysis_runs();
        REQUIRE(runs.has_value());
        CHECK(runs->front().state == "paused");
        CHECK(runs->front().paused == 2);
        CHECK(runs->front().running == 1);
        CHECK(runs->front().queued == 0);
        CHECK(runs->front().blocked == 0);
        auto nothing = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(nothing.has_value());
        CHECK_FALSE(nothing->has_value());

        const auto facts = [](const Library::JobLease& lease) {
            return Library::AnalysisFacts{.sample_id = lease.sample_id,
                                          .content_revision = lease.content_revision,
                                          .bpm_milli = 120'000,
                                          .tempo_algorithm = "test",
                                          .tempo_requested = true};
        };
        REQUIRE(library.complete_job(**leased, facts(**leased)).has_value());  // the leased job finishes
        CHECK(library.list_analysis_runs()->front().state == "paused");
        REQUIRE(library.control_run(run->id, Library::RunControl::Resume).has_value());
        CHECK(library.list_analysis_runs()->front().queued == 2);
        for (int remaining = 0; remaining < 2; ++remaining) {
            auto next = library.lease_next_job(std::chrono::seconds(15));
            REQUIRE(next.has_value());
            REQUIRE(next->has_value());
            REQUIRE(library.complete_job(**next, facts(**next)).has_value());
        }
        CHECK(library.list_analysis_runs()->front().state == "completed");

        // Paused while its only job is leased: when that job finishes nothing is left, so it completes.
        auto single = library.create_analysis_run({"Tempo", {"tempo"}, {all_sample_ids(library).front()}, true});
        REQUIRE(single.has_value());
        auto only = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(only.has_value());
        REQUIRE(only->has_value());
        REQUIRE(library.control_run(single->id, Library::RunControl::Pause).has_value());
        REQUIRE(library.complete_job(**only, facts(**only)).has_value());
        CHECK(library.list_analysis_runs()->front().state == "completed");
    }

    TEST_CASE("leased jobs of a paused run return as paused: on release, transient failure and lease expiry") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        auto run = library.create_analysis_run({"Tempo", {"tempo"}, all_sample_ids(library), false});
        REQUIRE(run.has_value());
        std::vector<Library::JobLease> leases;
        for (int job = 0; job < 3; ++job) {
            // The last lease is already expired, so the next lease_next_job reclaims it.
            const auto duration = job == 2 ? std::chrono::milliseconds(-1) : std::chrono::milliseconds(60'000);
            auto leased = library.lease_next_job(duration);
            REQUIRE(leased.has_value());
            REQUIRE(leased->has_value());
            leases.push_back(**leased);
        }
        REQUIRE(library.control_run(run->id, Library::RunControl::Pause).has_value());
        REQUIRE(library.release_job(leases[0]).has_value());
        REQUIRE(library.fail_job(leases[1], ErrorCode::CorruptAudio, /*transient=*/true).has_value());
        auto nothing = library.lease_next_job(std::chrono::seconds(15));  // reclaims the expired lease
        REQUIRE(nothing.has_value());
        CHECK_FALSE(nothing->has_value());
        for (const auto& lease : leases) {
            CAPTURE(lease.job_id);
            CHECK(job_state(library, lease.job_id) == "paused");
        }
        CHECK(query(library, "SELECT count(*) FROM jobs WHERE state IN ('queued','interrupted')") == 0);

        REQUIRE(library.control_run(run->id, Library::RunControl::Resume).has_value());
        auto runs = library.list_analysis_runs();
        REQUIRE(runs.has_value());
        CHECK(runs->front().queued == 3);
        CHECK(runs->front().paused == 0);
        // Outside a pause the same path still requeues.
        auto again = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(again.has_value());
        REQUIRE(again->has_value());
        REQUIRE(library.release_job(**again).has_value());
        CHECK(job_state(library, (*again)->job_id) == "queued");
    }

    TEST_CASE("a damaged job row fails on its own and the queue moves on") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        auto run = library.create_analysis_run({"Tempo", {"tempo"}, all_sample_ids(library), false});
        REQUIRE(run.has_value());
        const auto first_job = query(library, "SELECT min(id) FROM jobs");
        execute(library, std::format("UPDATE jobs SET uuid='not a uuid' WHERE id={}", first_job));
        execute(library, std::format("UPDATE jobs SET checkpoint_json='{{}}' WHERE id={}", first_job + 1));
        execute(library,
                std::format("UPDATE jobs SET checkpoint_json='{{}}', stage='bogus' WHERE id={}", first_job + 2));

        auto lease = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(lease.has_value());
        REQUIRE(lease->has_value());
        CHECK((*lease)->job_id == first_job + 1);
        CHECK((*lease)->stages == std::vector<std::string>{"tempo"});  // recovered from the stage column
        CHECK(job_state(library, first_job) == "failed");
        CHECK(query_text(library, std::format("SELECT error_code FROM jobs WHERE id={}", first_job)) == "DB_CORRUPT");
        auto empty = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(empty.has_value());
        CHECK_FALSE(empty->has_value());
        CHECK(job_state(library, first_job + 2) == "failed");
        REQUIRE(library.release_job(**lease).has_value());
    }

    TEST_CASE("loudness, PCM hash and waveform results are stored with their algorithm versions") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        Library::AnalysisFacts facts{.sample_id = kick,
                                     .content_revision = 1,
                                     .peak_dbfs = -1.0,
                                     .rms_dbfs = -12.0,
                                     .pcm_sha256 = std::string(64, 'a'),
                                     .waveform_segment = "derived/waveforms/kick.bin"};
        auto unversioned = library.record_analysis(facts);
        REQUIRE_FALSE(unversioned.has_value());
        CHECK(unversioned.error().code == ErrorCode::InvalidArgument);
        facts.loudness_algorithm = "loudness-test-3";
        facts.pcm_hash_algorithm = "pcm-hash-test-4";
        facts.waveform_algorithm = "waveform-test-5";
        REQUIRE(library.record_analysis(facts).has_value());
        CHECK(query_text(library, "SELECT algorithm_version FROM metadata_observations WHERE field='peak_dbfs'") ==
              "loudness-test-3");
        CHECK(query_text(library, "SELECT algorithm_version FROM metadata_observations WHERE field='pcm_sha256'") ==
              "pcm-hash-test-4");
        CHECK(query_text(library, "SELECT algorithm_version FROM artifacts WHERE kind='waveform'") ==
              "waveform-test-5");
    }

    TEST_CASE("opening a library written with another key normalization rebuilds its keys") {
        ScratchDirectory scratch;
        std::int64_t root = 0;
        {
            auto library = Library::create(scratch / "lib", "Keys");
            REQUIRE(library.has_value());
            root = add_scanned_root(**library, "Root", {test::synthetic_file("Drums/Kick.wav", 10, 1'000)}, false);
            REQUIRE((*library)->ensure_tag("Drums/Acoustic").has_value());
            execute(**library,
                    "UPDATE library_state SET normalization_version='older'; "
                    "UPDATE locations SET path_key='drums/kick.wav'; UPDATE folders SET path_key='drums'; "
                    "UPDATE samples SET name_key='stale'; UPDATE tags SET name_key='stale-'||id");
        }
        auto reopened = Library::open(scratch / "lib");
        REQUIRE_MESSAGE(reopened.has_value(), reopened.error().message());
        auto& library = **reopened;
        CHECK(query_text(library, "SELECT normalization_version FROM library_state") == kFoldVersion);
        CHECK(query_text(library, "SELECT path_key FROM locations") == path_key("Drums/Kick.wav", false));
        CHECK(query_text(library, "SELECT path_key FROM folders") == path_key("Drums", false));
        CHECK(query_text(library, "SELECT name_key FROM samples") == fold_key("Kick"));
        CHECK(query(library, "SELECT count(*) FROM tags WHERE name_key LIKE 'stale%'") == 0);
        auto stats = library.known_stats_in_directory(root, "DRUMS");
        REQUIRE(stats.has_value());
        CHECK(stats->contains(path_key("drums/kick.WAV", false)));
        CHECK(library.ensure_tag("drums/acoustic").has_value());
        CHECK(query(library, "SELECT count(*) FROM tags") == 2);  // found, not created again
    }

    TEST_CASE("a key rebuild with collisions rekeys everything else, keeps colliding rows apart and lists them") {
        ScratchDirectory scratch;
        std::int64_t root = 0;
        {
            auto library = Library::create(scratch / "lib", "Keys");
            REQUIRE(library.has_value());
            const std::vector<ScannedFile> files = {
                test::synthetic_file("Kick.wav", 10, 1'000), test::synthetic_file("kick.wav", 20, 1'000),
                test::synthetic_file("Snare.wav", 30, 1'000), test::synthetic_file("A.wav", 40, 1'000),
                test::synthetic_file("B.wav", 50, 1'000)};
            root = add_scanned_root(**library, "Root", files, /*case_sensitive=*/true);
            REQUIRE((*library)->ensure_tag("Bass").has_value());
            REQUIRE((*library)->ensure_tag("Kick").has_value());
            // As if the root had been stored case-insensitive under a key scheme that kept Kick.wav
            // and kick.wav apart; A.wav and B.wav hold each other's new keys (they must swap); the tag
            // "Kick" was renamed "bass" outside the app, so its new key is the tag "Bass"'s key.
            execute(
                **library,
                "UPDATE roots SET case_sensitive=0; UPDATE library_state SET normalization_version='older'; "
                "UPDATE locations SET path_key='swap' WHERE relative_path='A.wav'; "
                "UPDATE locations SET path_key='A.WAV' WHERE relative_path='B.wav'; "
                "UPDATE locations SET path_key='B.WAV' WHERE relative_path='A.wav'; "
                "UPDATE locations SET path_key='old-'||relative_path WHERE relative_path IN ('Kick.wav','Snare.wav'); "
                "UPDATE tags SET name='bass' WHERE name='Kick'");
        }
        std::vector<OpenProgress> progress;
        auto reopened = Library::open(scratch / "lib",
                                      {.on_progress = [&](const OpenProgress& step) { progress.push_back(step); }});
        REQUIRE_MESSAGE(reopened.has_value(), reopened.error().message());
        auto& library = **reopened;
        CHECK(query_text(library, "SELECT normalization_version FROM library_state") == kFoldVersion);
        const auto key_of = [&](std::string_view relative_path) {
            return query_text(library,
                              std::format("SELECT path_key FROM locations WHERE relative_path='{}'", relative_path));
        };
        CHECK(key_of("Snare.wav") == "SNARE.WAV");
        CHECK(key_of("A.wav") == "A.WAV");  // the swap settled
        CHECK(key_of("B.wav") == "B.WAV");
        // Exactly one of Kick.wav and kick.wav has the new key; the other keeps a placeholder.
        CHECK(query(library, "SELECT count(*) FROM locations WHERE path_key='KICK.WAV'") == 1);
        CHECK(query(library, "SELECT count(*) FROM locations WHERE substr(path_key,1,1)=char(1)") == 1);
        CHECK(query(library, "SELECT count(*) FROM tags WHERE substr(name_key,1,1)=char(1)") == 1);
        CHECK(query(library, "SELECT count(*) FROM samples") == 5);  // nothing merged

        const auto& report = library.recovery_report();
        CHECK(report.key_conflicts == 2);
        REQUIRE(report.pre_rekey_backup.has_value());
        CHECK(std::filesystem::exists(*report.pre_rekey_backup));
        auto review = library.open_review_items();
        REQUIRE(review.has_value());
        REQUIRE(std::ranges::count(*review, std::string("normalization_conflict"), &ReviewItem::kind) == 1);
        const auto item = std::ranges::find(*review, std::string("normalization_conflict"), &ReviewItem::kind);
        const auto evidence = nlohmann::json::parse(item->evidence_json);
        CHECK(evidence.at("conflict_count").get<int>() == 2);
        CHECK(evidence.at("conflicts").size() == 2);
        // The rebuild reported its parts in order and finished.
        REQUIRE_FALSE(progress.empty());
        CHECK(progress.front().step == OpenStep::BackingUp);
        CHECK(progress.back().step == OpenStep::RebuildingKeys);
        CHECK(progress.back().completed == progress.back().total);

        // The point of rekeying the rest: a scan of the case-insensitive root finds every file it
        // already knows by its new key, so nothing is added twice and nothing goes missing.
        auto session = library.begin_scan(root);
        REQUIRE(session.has_value());
        const std::vector<ScannedFile> rescanned = {test::synthetic_file("SNARE.wav", 30, 1'000),
                                                    test::synthetic_file("A.wav", 40, 1'000),
                                                    test::synthetic_file("B.wav", 50, 1'000)};
        auto committed = library.commit_scan_batch(*session, rescanned, {}, [](std::string_view) { return true; });
        REQUIRE(committed.has_value());
        CHECK(committed->added == 0);
        CHECK(query(library, "SELECT count(*) FROM samples") == 5);

        // The next open has nothing to rebuild and keeps the one review item.
        reopened->reset();
        auto again = Library::open(scratch / "lib");
        REQUIRE(again.has_value());
        CHECK((*again)->recovery_report().key_conflicts == 0);
        CHECK_FALSE((*again)->recovery_report().pre_rekey_backup.has_value());
        review = (*again)->open_review_items();
        REQUIRE(review.has_value());
        CHECK(std::ranges::count(*review, std::string("normalization_conflict"), &ReviewItem::kind) == 1);
    }

    TEST_CASE("a stopped backup leaves no file and reports CANCELLED; a running one does not block writers") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        std::stop_source stopped;
        stopped.request_stop();
        auto cancelled = library.create_backup("manual", stopped.get_token());
        REQUIRE_FALSE(cancelled.has_value());
        CHECK(cancelled.error().code == ErrorCode::Cancelled);
        const auto backup_files = [&] {
            std::size_t count = 0;
            for (const auto& entry : std::filesystem::directory_iterator(scratch / "lib/backups"))
                count += entry.is_regular_file() ? 1 : 0;
            return count;
        };
        CHECK(backup_files() == 0);

        // The two long phases stop on request: the page copy (checked before every step) and the
        // verification of the copy (SQLite's progress handler interrupts the integrity check).
        auto reader = library.acquire_reader();
        REQUIRE(reader.has_value());
        auto& source = reader->connection();
        {
            // A snapshot like create_backup's: the writer keeps committing while it is open.
            sqlite::ReadTransaction snapshot(source);
            REQUIRE(snapshot.begin().has_value());
            REQUIRE(source.query_int64("SELECT count(*) FROM sqlite_schema").has_value());
            REQUIRE(library.apply_edit(rate({test::sample_id_named(library, "Snare")}, 4)).has_value());
            auto copied = source.backup_to(scratch / "copy.sqlite", 1, std::chrono::seconds(1), std::stop_token{});
            REQUIRE(copied.has_value());
            auto refused =
                source.backup_to(scratch / "refused.sqlite", 1, std::chrono::seconds(1), stopped.get_token());
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().code == ErrorCode::Cancelled);
        }
        auto copy = sqlite::Connection::open(scratch / "copy.sqlite", {.read_only = true, .write_ahead_log = false});
        REQUIRE(copy.has_value());
        CHECK(copy->query_int64("SELECT count(*) FROM samples WHERE rating=4").value_or(-1) == 0);  // the snapshot
        CHECK(copy->integrity_check().value_or("") == "ok");
        auto interrupted = copy->integrity_check(stopped.get_token());
        REQUIRE_FALSE(interrupted.has_value());
        CHECK(interrupted.error().code == ErrorCode::Cancelled);
        auto interrupted_keys = copy->foreign_key_violations(stopped.get_token());
        REQUIRE_FALSE(interrupted_keys.has_value());
        CHECK(interrupted_keys.error().code == ErrorCode::Cancelled);

        auto backup = library.create_backup("manual");
        REQUIRE(backup.has_value());
        CHECK(backup_files() == 1);
    }

    TEST_CASE("an upgrade parks the waiting jobs of paused runs and settles paused runs with nothing left") {
        ScratchDirectory scratch;
        const auto uuid = new_uuid();
        write_library_at_version(scratch / "lib", uuid, 5);
        {
            auto connection = sqlite::Connection::open(scratch / "lib" / Library::kCatalogFileName, {});
            REQUIRE(connection.has_value());
            REQUIRE(connection
                        ->execute_script(
                            "INSERT INTO samples(id,uuid,display_name,name_key,created_at,updated_at) "
                            "VALUES(1,'s1','One','one',0,0);"
                            "INSERT INTO analysis_profiles(id,uuid,version,name,config_json,digest,created_at) "
                            "VALUES(1,'p',1,'Tempo','{}','d',0);"
                            "INSERT INTO analysis_runs(id,uuid,profile_id,state,requested_at,input_revision,"
                            "selection_json) VALUES(1,'r1',1,'paused',0,0,'{}'),(2,'r2',1,'paused',0,0,'{}');"
                            "INSERT INTO jobs(id,uuid,run_id,sample_id,content_revision,stage,state) "
                            "VALUES(1,'j1',1,1,1,'tempo','queued'),(2,'j2',2,1,1,'tempo','completed')")
                        .has_value());
            connection->close();
        }
        auto library = Library::open(scratch / "lib");
        REQUIRE_MESSAGE(library.has_value(), library.error().message());
        CHECK(job_state(**library, 1) == "paused");
        CHECK(query_text(**library, "SELECT state FROM analysis_runs WHERE id=1") == "paused");
        CHECK(query_text(**library, "SELECT state FROM analysis_runs WHERE id=2") == "completed");
        CHECK(query(**library, "SELECT count(*) FROM pragma_index_list('jobs') WHERE name='jobs_ready'") == 1);
    }

    TEST_CASE("the integrity report notices a damaged search index") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        auto healthy = library.integrity_report();
        REQUIRE(healthy.has_value());
        CHECK(*healthy == "integrity: ok; foreign keys: 0 violation(s); search index: ok");
        // The search projection changes behind the index's back (the database itself stays sound).
        execute(library, "DROP TRIGGER sample_search_au; UPDATE sample_search SET name='renamed elsewhere'");
        auto report = library.integrity_report();
        REQUIRE(report.has_value());
        CAPTURE(*report);
        CHECK(report->starts_with("integrity: ok; foreign keys: 0 violation(s); search index: "));
        CHECK_FALSE(report->ends_with("search index: ok"));
    }

    // ------------------------------------------------------------ third review round

    TEST_CASE("every statement the catalog runs on locations and jobs is index-driven: no full scan, no sort") {
        // Theme 1 of the third review: a statement can miss the partial index it was written for
        // (SQLite does not infer `state<>'retired'` from `state='missing'`). This exercises scans,
        // availability, edits, history and the analysis queue, then explains every statement the
        // library's connections compiled. Plans come from SQLite's built-in estimates (catalogs are
        // never ANALYZEd), so they are the same here as in a library with a million locations.
        ScratchDirectory scratch;
        auto created = Library::create(scratch / "lib", "Plans");
        REQUIRE(created.has_value());
        auto& library = **created;
        auto moved = test::synthetic_file("Old/Moved.wav", 10, 1'000);
        moved.volume_id = "volume";
        moved.file_id = std::string(16, 'x');
        const std::vector<ScannedFile> first_files = {
            test::synthetic_file("Drums/Kick.wav", 20, 1'000), test::synthetic_file("Drums/Snare.wav", 30, 1'000),
            test::synthetic_file("Cloud/Pad.wav", 40, 1'000), test::synthetic_file("Gone/Old.wav", 50, 1'000), moved};
        const auto root = add_scanned_root(library, "Root", first_files, /*case_sensitive=*/false);
        const auto none = [](std::string_view) { return false; };

        // A rescan with a rename, a changed file, a new file, skipped entries and a gap, then one more
        // scan that brings the missing file back as a skipped entry, and a cancelled scan.
        auto rescan = library.begin_scan(root);
        REQUIRE(rescan.has_value());
        auto renamed = moved;
        renamed.relative_path = "New/Moved.wav";
        const std::vector<ScannedFile> second_files = {test::synthetic_file("Drums/Kick.wav", 21, 1'000), renamed,
                                                       test::synthetic_file("Drums/Clap.wav", 60, 1'000)};
        REQUIRE(library.commit_scan_batch(*rescan, second_files, std::vector<std::string>{"Drums"}, none).has_value());
        REQUIRE(library.mark_scan_entries_seen(*rescan, std::vector<std::string>{"Cloud/Pad.wav"}).has_value());
        REQUIRE(library.record_scan_gap(*rescan, "Denied", /*denied=*/true).has_value());
        REQUIRE(library.completed_directories(*rescan).has_value());
        REQUIRE(library.known_stats_in_directory(root, "").has_value());
        REQUIRE(library.known_stats_in_directory(root, "Drums").has_value());
        REQUIRE(library.finish_scan(*rescan, false).has_value());
        auto revive = library.begin_scan(root);
        REQUIRE(revive.has_value());
        auto all_present = second_files;
        all_present.push_back(test::synthetic_file("Drums/Snare.wav", 30, 1'000));
        REQUIRE(library.commit_scan_batch(*revive, all_present, {}, none).has_value());
        REQUIRE(library.mark_scan_entries_seen(*revive, std::vector<std::string>{"Gone/Old.wav", "Cloud/Pad.wav"})
                    .has_value());
        REQUIRE(library.finish_scan(*revive, false).has_value());
        auto cancelled = library.begin_scan(root);
        REQUIRE(cancelled.has_value());
        REQUIRE(library.finish_scan(*cancelled, true).has_value());
        REQUIRE(library.set_root_availability(root, Availability::Offline).has_value());
        REQUIRE(library.set_root_availability(root, Availability::Online).has_value());
        REQUIRE(library.rename_root(root, "Renamed").has_value());
        REQUIRE(library.list_folders(root).has_value());
        REQUIRE(library.list_roots().has_value());
        REQUIRE(library.list_roots(RootListing::WithoutSampleCounts).has_value());

        // Edits, history and reads of one sample.
        const auto samples = all_sample_ids(library);
        REQUIRE(library.sample_detail(samples.front()).has_value());
        EditCommand tag_edit = rate(samples, 4);
        tag_edit.tags_add = {"Drums"};
        REQUIRE(library.apply_edit(tag_edit).has_value());
        REQUIRE(library.undo().has_value());
        REQUIRE(library.redo().has_value());
        REQUIRE(library.changes_since(0).has_value());

        // The analysis queue: both kinds of run, every lease outcome and every run control.
        auto run = library.create_analysis_run({"Plans", {"tempo", "byte_hash"}, samples, true});
        REQUIRE(run.has_value());
        REQUIRE(library.create_analysis_run_for_root(root, "Root plans", {"tempo"}, true).has_value());
        const auto facts = [](const Library::JobLease& lease) {
            return Library::AnalysisFacts{.sample_id = lease.sample_id,
                                          .content_revision = lease.content_revision,
                                          .bpm_milli = 120'000,
                                          .byte_sha256 = std::string(64, 'a'),
                                          .tempo_algorithm = "test",
                                          .tempo_requested = true};
        };
        const auto lease = [&] {
            auto next = library.lease_next_job(std::chrono::seconds(15));
            REQUIRE(next.has_value());
            REQUIRE(next->has_value());
            return **next;
        };
        auto completed = lease();
        REQUIRE(library.renew_lease(completed, std::chrono::seconds(15)).has_value());
        REQUIRE(library.complete_job(completed, facts(completed)).has_value());
        REQUIRE(library.fail_job(lease(), ErrorCode::CorruptAudio, /*transient=*/true).has_value());
        REQUIRE(library.fail_job(lease(), ErrorCode::SourceOffline, /*transient=*/false).has_value());
        REQUIRE(library.release_job(lease()).has_value());
        REQUIRE(library.control_run(run->id, Library::RunControl::Pause).has_value());
        REQUIRE(library.control_run(run->id, Library::RunControl::Resume).has_value());
        REQUIRE(library.control_run(run->id, Library::RunControl::Cancel).has_value());
        REQUIRE(library.list_analysis_runs().has_value());

        // Plans that read more than the rows they need, each with the reason it is bounded anyway.
        struct Accepted {
            std::string_view statement;  ///< a fragment that identifies the statement
            std::string_view plan_line;  ///< the EXPLAIN QUERY PLAN line that is accepted for it
            std::string_view reason;
        };
        constexpr std::array<Accepted, 6> kAccepted = {{
            {"FROM jobs INDEXED BY jobs_ready", "SCAN jobs USING INDEX jobs_ready",
             "the lease walks the ready jobs in lease order and stops at the first leasable one"},
            {"INSERT INTO jobs(", "USE TEMP B-TREE FOR ORDER BY",
             "run creation sorts its own selection once, so job ids follow sample ids"},
            {"INSERT INTO sample_search(", "USE TEMP B-TREE FOR group_concat(ORDER BY)",
             "orders the few locations and tags of one sample"},
            {"WHERE location.sample_id=?1 AND location.state<>'retired' ORDER BY location.id",
             "USE TEMP B-TREE FOR ORDER BY", "orders the locations of one sample"},
            {"FROM roots root WHERE root.deleted=0 ORDER BY root.name", "USE TEMP B-TREE FOR ORDER BY",
             "sorts the handful of roots by name"},
            {"count(DISTINCT location.sample_id)", "USE TEMP B-TREE FOR count(DISTINCT)",
             "per-root sample counts are opt-in (RootListing::WithSampleCounts): one pass over the root's locations"},
        }};

        // Nothing was dropped from a full cache, so every statement that ran is in the set below.
        CHECK(writer(library).cached_statement_count() < sqlite::StatementCache::kMaximumIdleStatements);
        const auto statements = cached_statements(library);
        std::size_t checked = 0;
        bool saw_revive = false;
        bool saw_lease = false;
        for (const auto& sql : statements) {
            if (covered_table_names(sql).empty()) continue;
            ++checked;
            saw_revive = saw_revive || sql.contains("state<>'retired' AND state='missing'");
            saw_lease = saw_lease || sql.contains("INDEXED BY jobs_ready");
            auto findings = full_scans_and_sorts(writer(library), sql);
            std::erase_if(findings, [&](const std::string& line) {
                return std::ranges::any_of(kAccepted, [&](const Accepted& accepted) {
                    return sql.contains(accepted.statement) && line == accepted.plan_line;
                });
            });
            CAPTURE(sql);
            CHECK_MESSAGE(findings.empty(), std::format("{}", findings));
        }
        MESSAGE(std::format("{} statements on locations or jobs explained", checked));
        CHECK(checked >= 40);
        CHECK(saw_revive);
        CHECK(saw_lease);
    }

    TEST_CASE("benchmark: leasing at 100k queued jobs and finishing a scan with many directories" *
              doctest::skip(!test::environment_flag("MSO_CATALOG_BENCHMARK"))) {
        ScratchDirectory scratch;
        auto created = Library::create(scratch / "lib", "Bench");
        REQUIRE(created.has_value());
        auto& library = **created;
        constexpr int kSamples = 100'000;
        constexpr int kDirectories = 5'000;
        const auto root = library.add_root("Bench", RootMode::Referenced, false);
        REQUIRE(root.has_value());
        execute(library, std::format("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<{0}) "
                                     "INSERT INTO samples(id,uuid,display_name,name_key,created_at,updated_at) "
                                     "SELECT i, 'sample-'||i, 'S'||i, 's'||i, 0, 0 FROM n;"
                                     "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<{0}) "
                                     "INSERT INTO locations(sample_id,root_id,relative_path,path_key,state,"
                                     "observed_content_revision,seen_scan_generation) "
                                     "SELECT i, {1}, 'D'||(i%{2})||'/S'||i||'.wav', 'D'||(i%{2})||'/S'||i||'.WAV', "
                                     "'present', 1, 0 FROM n;",
                                     kSamples, root->id, kDirectories));
        REQUIRE(library.set_root_availability(root->id, Availability::Online).has_value());
        auto run = library.create_analysis_run_for_root(root->id, "Bench", {"tempo"}, false);
        REQUIRE(run.has_value());
        REQUIRE(run->queued == kSamples);

        using Clock = std::chrono::steady_clock;
        const auto milliseconds_per = [](Clock::duration elapsed, int count) {
            return std::chrono::duration<double, std::milli>(elapsed).count() / count;
        };
        // Before: the review-time query (sorts every queued job in a temp B-tree per lease).
        constexpr std::string_view kOldLeaseQuery =
            "SELECT jobs.id FROM jobs JOIN analysis_runs run ON run.id=jobs.run_id "
            "WHERE jobs.state IN ('queued','interrupted') AND run.state IN ('queued','running') "
            "AND (jobs.lease_expires_at IS NULL OR jobs.lease_expires_at<=?1) AND {} "
            "ORDER BY run.id, jobs.priority DESC, jobs.id LIMIT 1";
        constexpr int kOldLeases = 20;
        auto old_query = writer(library).prepare(std::format(kOldLeaseQuery, detail::kJobSampleReachableSql));
        REQUIRE(old_query.has_value());
        auto started = Clock::now();
        for (int lease = 0; lease < kOldLeases; ++lease) {
            old_query->bind(1, now_unix_ms());
            REQUIRE(old_query->next_row().value_or(false));
            old_query->reset();
        }
        const double old_per_lease = milliseconds_per(Clock::now() - started, kOldLeases);

        constexpr int kLeases = 2'000;
        started = Clock::now();
        for (int lease = 0; lease < kLeases; ++lease) {
            auto next = library.lease_next_job(std::chrono::minutes(10));
            REQUIRE(next.has_value());
            REQUIRE(next->has_value());
        }
        const double new_per_lease = milliseconds_per(Clock::now() - started, kLeases);
        MESSAGE(
            std::format("lease at {} queued jobs: before {:.3f} ms (query only), after {:.3f} ms per "
                        "lease_next_job (whole transaction)",
                        kSamples, old_per_lease, new_per_lease));

        // finish_scan: 20k unseen locations under 2k gap-free directories plus one denied directory.
        auto session = library.begin_scan(root->id);
        REQUIRE(session.has_value());

        // mark_scan_entries_seen: one full batch of skipped paths (cloud placeholders, links) in the
        // 100k-location root, half of them missing since an earlier scan. All above sample 50 000,
        // so the finish_scan numbers below are unchanged.
        constexpr int kSkipped = static_cast<int>(Library::kMaximumScanBatchFiles);
        std::vector<std::string> skipped_paths;
        for (int index = 0; index < kSkipped; ++index) {
            const int sample = 50'001 + index * 97;
            skipped_paths.push_back(std::format("D{}/S{}.wav", sample % kDirectories, sample));
        }
        execute(library, "UPDATE locations SET state='missing' WHERE sample_id>50000 AND sample_id%2=0");
        {
            // Before: the review-time revive statement, without the partial index's predicate.
            constexpr std::string_view kOldRevive =
                "UPDATE locations SET state='unknown', seen_scan_generation=?3, last_seen_at=?4 "
                "WHERE root_id=?1 AND path_key=?2 AND state='missing' RETURNING sample_id";
            auto old_revive = writer(library).prepare(kOldRevive);
            REQUIRE(old_revive.has_value());
            REQUIRE(writer(library).begin_immediate().has_value());
            started = Clock::now();
            for (const auto& path : skipped_paths) {
                old_revive->bind(1, root->id).bind(2, path_key(path, false)).bind(3, session->generation).bind(4, 0);
                while (old_revive->next_row().value_or(false)) {
                }
                old_revive->reset();
            }
            const double old_skip_batch = milliseconds_per(Clock::now() - started, 1);
            writer(library).rollback();
            started = Clock::now();
            REQUIRE(library.mark_scan_entries_seen(*session, skipped_paths).has_value());
            const double new_skip_batch = milliseconds_per(Clock::now() - started, 1);
            CHECK(query(library, "SELECT count(*) FROM locations WHERE state='unknown'") == kSkipped / 2);
            MESSAGE(
                std::format("mark_scan_entries_seen, {} skipped paths at {} locations: before {:.1f} ms (revive "
                            "statement alone), after {:.1f} ms (whole call)",
                            kSkipped, kSamples, old_skip_batch, new_skip_batch));
        }
        execute(library, std::format("WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i+1 FROM n WHERE i<{}) "
                                     "INSERT INTO scan_frontier(scan_id,relative_directory,state) "
                                     "SELECT {}, 'D'||i, 'complete' FROM n",
                                     kDirectories - 1, session->id));
        execute(library,
                std::format("UPDATE locations SET seen_scan_generation={} WHERE sample_id>20000", session->generation));
        REQUIRE(library.record_scan_gap(*session, "Elsewhere", /*denied=*/true).has_value());
        constexpr std::string_view kOldUnseenCount =
            "SELECT count(*) FROM locations location WHERE location.root_id={} "
            "AND location.state IN ('present','changed','unknown') AND location.seen_scan_generation<{} "
            "AND NOT EXISTS (SELECT 1 FROM scan_frontier gap NOT INDEXED WHERE gap.scan_id={} "
            "AND gap.state IN ('denied','failed') AND (gap.relative_directory='' OR "
            "substr(location.relative_path, 1, length(gap.relative_directory)+1)=gap.relative_directory || '/'))";
        started = Clock::now();
        CHECK(query(library, std::format(kOldUnseenCount, root->id, session->generation, session->id)) == 20'000);
        const double old_subquery = milliseconds_per(Clock::now() - started, 1);
        started = Clock::now();
        auto finished = library.finish_scan(*session, false);
        const double new_finish = milliseconds_per(Clock::now() - started, 1);
        REQUIRE(finished.has_value());
        CHECK(finished->marked_missing == 20'000);
        MESSAGE(
            std::format("finish_scan, 20k unseen of {} in {} directories: before {:.1f} ms (one of its two "
                        "unseen passes), after {:.1f} ms (whole finish_scan)",
                        kSamples, kDirectories, old_subquery, new_finish));

        // list_roots, with and without the per-root sample counts.
        constexpr int kListings = 10;
        started = Clock::now();
        for (int listing = 0; listing < kListings; ++listing)
            REQUIRE(library.list_roots().has_value());
        const double counted_listing = milliseconds_per(Clock::now() - started, kListings);
        started = Clock::now();
        for (int listing = 0; listing < kListings; ++listing)
            REQUIRE(library.list_roots(RootListing::WithoutSampleCounts).has_value());
        const double plain_listing = milliseconds_per(Clock::now() - started, kListings);
        MESSAGE(std::format("list_roots at {} locations: {:.2f} ms with sample counts, {:.3f} ms without", kSamples,
                            counted_listing, plain_listing));

        // Key rebuild on open, with every location and folder key stale (all of them change).
        constexpr std::string_view kStaleKeys =
            "UPDATE library_state SET normalization_version='older'; "
            "UPDATE locations SET path_key=lower(path_key); UPDATE folders SET path_key=lower(path_key)";
        execute(library, std::string(kStaleKeys));
        {
            // Before: the review-time two-pass rebuild of locations and folders (rolled back).
            constexpr std::array<std::string_view, 4> kOldRekey = {
                "UPDATE folders SET path_key=char(1)||id WHERE path_key IS NOT "
                "path_key(relative_path,(SELECT case_sensitive FROM roots WHERE roots.id=folders.root_id))",
                "UPDATE folders SET path_key=path_key(relative_path,(SELECT case_sensitive FROM roots "
                "WHERE roots.id=folders.root_id)) WHERE substr(path_key,1,1)=char(1)",
                "UPDATE locations SET path_key=char(1)||id WHERE path_key IS NOT "
                "path_key(relative_path,(SELECT case_sensitive FROM roots WHERE roots.id=locations.root_id))",
                "UPDATE locations SET path_key=path_key(relative_path,(SELECT case_sensitive FROM roots "
                "WHERE roots.id=locations.root_id)) WHERE substr(path_key,1,1)=char(1)"};
            REQUIRE(writer(library).begin_immediate().has_value());
            started = Clock::now();
            for (const auto sql : kOldRekey)
                REQUIRE(writer(library).execute_script(sql).has_value());
            const double old_rekey = milliseconds_per(Clock::now() - started, 1);
            writer(library).rollback();
            const auto directory = library.directory();
            created->reset();
            std::optional<Clock::time_point> rebuild_started;
            std::optional<Clock::time_point> rebuild_finished;
            const auto open_started = Clock::now();
            auto reopened = Library::open(directory, {.on_progress = [&](const OpenProgress& progress) {
                                              if (progress.step != OpenStep::RebuildingKeys) return;
                                              if (!rebuild_started) rebuild_started = Clock::now();
                                              if (progress.completed == progress.total) rebuild_finished = Clock::now();
                                          }});
            const double whole_open = milliseconds_per(Clock::now() - open_started, 1);
            REQUIRE(reopened.has_value());
            REQUIRE(rebuild_started.has_value());
            REQUIRE(rebuild_finished.has_value());
            CHECK(query(**reopened, "SELECT count(*) FROM locations WHERE path_key<>upper(path_key)") == 0);
            MESSAGE(
                std::format("key rebuild of {} locations and {} folders: before {:.0f} ms (two-pass location "
                            "and folder statements), after {:.0f} ms (whole rebuild, all tables); open with "
                            "its verified backup {:.0f} ms",
                            kSamples, kDirectories, old_rekey,
                            milliseconds_per(*rebuild_finished - *rebuild_started, 1), whole_open));
        }
    }

}  // TEST_SUITE("catalog")
