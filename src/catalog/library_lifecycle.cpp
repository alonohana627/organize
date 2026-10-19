// Library creation, opening, migrations, crash recovery, reader pool and backups.
#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <sstream>

#include "mso/catalog/detail/catalog_internal.hpp"
#include "mso/catalog/detail/reader_pool.hpp"
#include "mso/catalog/durable_file.hpp"
#include "mso/catalog/library.hpp"
#include "mso/catalog/migrations.hpp"

namespace mso::catalog {

namespace {

constexpr std::string_view kBackupDirectory = "backups";
constexpr std::string_view kStagingDirectory = "staging";
constexpr std::size_t kMaximumBackupReasonLength = 48;
constexpr std::array<std::string_view, 5> kLibrarySubdirectories = {"media", "derived/waveforms", "derived/features",
                                                                    "backups", "staging"};

/// The identity manifest stored next to the database.
struct LibraryManifest {
    uuids::uuid uuid;
    std::string name;
    int format_version = 0;
};

Result<LibraryManifest> read_manifest(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail(ErrorCode::NotFound, std::format("no library manifest at {}", path_to_utf8(file)));
    std::stringstream contents;
    contents << in.rdbuf();
    const auto document = json_fields::parse_json(contents.str());
    if (document.is_discarded() || !document.is_object())
        return fail(ErrorCode::DbCorrupt, "library.json is not valid JSON");
    LibraryManifest manifest;
    const auto uuid_text = json_fields::string_field(document, "library_uuid");
    const auto uuid = uuid_text ? uuids::uuid::from_string(*uuid_text) : std::nullopt;
    if (!uuid) return fail(ErrorCode::DbCorrupt, "library.json has no valid library_uuid");
    manifest.uuid = *uuid;
    manifest.name = json_fields::string_field(document, "name").value_or("");
    const auto format_version = json_fields::integer_field(document, "format_version");
    if (!format_version || *format_version < 0 || *format_version > 1'000'000)
        return fail(ErrorCode::DbCorrupt, "library.json has no valid format_version");
    manifest.format_version = static_cast<int>(*format_version);
    return manifest;
}

Result<void> write_manifest(const std::filesystem::path& file, const LibraryManifest& manifest) {
    const nlohmann::json document = {{"format_version", manifest.format_version},
                                     {"library_uuid", uuids::to_string(manifest.uuid)},
                                     {"name", manifest.name},
                                     {"application", "Music Sample Organizer"}};
    return write_file_atomically(file, document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace));
}

/// Verifies a backup copy: integrity check, foreign keys and identity.
Result<void> verify_backup(const std::filesystem::path& file, const uuids::uuid& expected) {
    auto connection = sqlite::Connection::open(file, {.read_only = true, .write_ahead_log = false});
    if (!connection) return std::unexpected(connection.error());
    auto integrity = connection->integrity_check();
    if (!integrity) return std::unexpected(integrity.error());
    if (*integrity != "ok") return fail(ErrorCode::DbCorrupt, std::format("backup integrity check: {}", *integrity));
    auto violations = connection->foreign_key_violations();
    if (!violations) return std::unexpected(violations.error());
    if (!violations->empty())
        return fail(ErrorCode::DbCorrupt, std::format("backup has {} foreign key violations", violations->size()));
    auto statement = connection->prepare("SELECT library_uuid FROM library_state WHERE singleton=1");
    if (!statement) return std::unexpected(statement.error());
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row || statement->column_text(0) != uuids::to_string(expected))
        return fail(ErrorCode::DbCorrupt, "backup identity does not match the library");
    return {};
}

/// Highest applied migration version, or 0 for an empty database.
Result<int> applied_schema_version(sqlite::Connection& connection) {
    auto has_table =
        connection.query_int64("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='schema_migrations'");
    if (!has_table) return std::unexpected(has_table.error());
    if (*has_table == 0) return 0;
    auto version = connection.query_int64("SELECT COALESCE(max(version),0) FROM schema_migrations");
    if (!version) return std::unexpected(version.error());
    return static_cast<int>(*version);
}

/// Released migrations must be byte-identical to what this build ships, and applied without gaps
/// (1, 2, 3, …). Versions newer than this build are allowed (the library then opens read-only).
Result<void> verify_migration_checksums(sqlite::Connection& connection) {
    auto statement = connection.prepare("SELECT version, checksum FROM schema_migrations ORDER BY version");
    if (!statement) return std::unexpected(statement.error());
    int expected_version = 1;
    return detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        const auto version = row.column_int64(0);
        if (version != expected_version)
            return fail(ErrorCode::DbCorrupt,
                        std::format("schema migration {} is missing (found {} next)", expected_version, version));
        ++expected_version;
        for (const auto& migration : migrations()) {
            if (migration.version == version && migration.checksum != row.column_text(1))
                return fail(ErrorCode::DbCorrupt,
                            std::format("migration {} checksum differs from this build", version));
        }
        return {};
    });
}

/// Runs one migration's script and records it, inside the caller's transaction.
Result<void> apply_migration_body(sqlite::Connection& connection, const Migration& migration) {
    if (auto applied = connection.execute_script(migration.sql); !applied) return applied;
    auto record = connection.prepare("INSERT INTO schema_migrations(version,checksum,applied_at) VALUES(?1,?2,?3)");
    if (!record) return std::unexpected(record.error());
    record->bind(1, migration.version).bind(2, migration.checksum).bind(3, now_unix_ms());
    if (auto recorded = record->run(); !recorded) return recorded;
    auto update = connection.prepare("UPDATE library_state SET schema_version=?1 WHERE singleton=1");
    if (!update) return std::unexpected(update.error());
    update->bind(1, migration.version);
    if (auto updated = update->run(); !updated) return updated;
    return connection.execute_script(std::format("PRAGMA user_version={}", migration.version));
}

/// Applies one pending migration of an existing library in its own transaction.
Result<void> apply_migration(sqlite::Connection& connection, const Migration& migration) {
    sqlite::WriteTransaction transaction(connection);
    if (auto began = transaction.begin(); !began) return began;
    if (auto applied = apply_migration_body(connection, migration); !applied) return applied;
    return transaction.commit();
}

/// Removes temporary files left by interrupted operations in staging/ (only our own *.partial).
/// Best effort: a file that cannot be removed now is retried at the next open.
void sweep_staging(const std::filesystem::path& staging) {
    std::error_code error;
    if (!std::filesystem::is_directory(staging, error)) return;
    std::filesystem::directory_iterator entries(staging, error);
    for (; !error && entries != std::filesystem::directory_iterator(); entries.increment(error)) {
        std::error_code entry_error;
        if (entries->is_regular_file(entry_error) && entries->path().extension() == ".partial")
            std::filesystem::remove(entries->path(), entry_error);
    }
}

/// A backup reason reduced to [A-Za-z0-9_-] (others become '-'), at most 48 characters.
std::string sanitized_backup_reason(std::string_view reason) {
    std::string sanitized;
    for (const char character : reason.substr(0, kMaximumBackupReasonLength)) {
        const auto byte = static_cast<unsigned char>(character);
        sanitized.push_back(std::isalnum(byte) != 0 || character == '-' || character == '_' ? character : '-');
    }
    return sanitized.empty() ? std::string("backup") : sanitized;
}

/// True for files named by create_backup(): "<13-digit ms>-<reason>-<id>.sqlite".
bool is_backup_file_name(const std::string& name) {
    return name.size() > 21 && name.ends_with(".sqlite") &&
           std::all_of(name.begin(), name.begin() + 13,
                       [](char character) { return character >= '0' && character <= '9'; }) &&
           name[13] == '-';
}

/// Deletes all but the newest `keep` backups. Best effort: a backup that cannot be deleted now (e.g.
/// open in another program) is retried after the next backup.
void prune_backups(const std::filesystem::path& directory, std::size_t keep) {
    std::error_code error;
    std::vector<std::filesystem::path> backups;
    std::filesystem::directory_iterator entries(directory, error);
    for (; !error && entries != std::filesystem::directory_iterator(); entries.increment(error)) {
        std::error_code entry_error;
        if (entries->is_regular_file(entry_error) && is_backup_file_name(path_to_utf8(entries->path().filename())))
            backups.push_back(entries->path());
    }
    if (backups.size() <= keep) return;
    std::ranges::sort(backups, std::greater<>{}, [](const std::filesystem::path& file) { return file.filename(); });
    for (std::size_t index = keep; index < backups.size(); ++index) {
        std::error_code remove_error;
        std::filesystem::remove(backups[index], remove_error);
    }
}

/// Raises `published` to `revision` unless another thread already published a newer one.
void publish_revision(std::atomic<std::int64_t>& published, std::int64_t revision) noexcept {
    std::int64_t current = published.load();
    while (current < revision && !published.compare_exchange_weak(current, revision)) {
        // compare_exchange_weak reloaded `current`; retry until ours is not newer or it is stored
    }
}

}  // namespace

// ---------------------------------------------------------------- ReaderLease

ReaderLease::ReaderLease(std::shared_ptr<detail::ReaderPool> pool, std::unique_ptr<sqlite::Connection> connection)
    : pool_(std::move(pool)), connection_(std::move(connection)) {}

ReaderLease::~ReaderLease() {
    if (pool_ && connection_) pool_->give_back(std::move(connection_));
}

// ---------------------------------------------------------------- creation and opening

Result<std::unique_ptr<Library>> Library::create(const std::filesystem::path& directory, std::string_view name) {
    std::error_code error;
    const bool existed = std::filesystem::exists(directory, error);
    if (error)
        return fail(ErrorCode::AccessDenied, std::format("cannot examine the library folder: {}", error.message()));
    if (existed) {
        const bool empty = std::filesystem::is_empty(directory, error);
        if (error)
            return fail(ErrorCode::AccessDenied, std::format("cannot read the library folder: {}", error.message()));
        if (!empty) return fail(ErrorCode::InvalidArgument, "the library folder must be new or empty");
    }

    // Everything below is undone on failure, so a failed creation never leaves a half library.
    auto remove_created = [&] {
        std::error_code cleanup_error;
        if (!existed) {
            std::filesystem::remove_all(directory, cleanup_error);
            return;
        }
        std::filesystem::directory_iterator entries(directory, cleanup_error);
        std::vector<std::filesystem::path> created;
        for (; !cleanup_error && entries != std::filesystem::directory_iterator(); entries.increment(cleanup_error))
            created.push_back(entries->path());
        for (const auto& entry : created) {
            std::error_code entry_error;
            std::filesystem::remove_all(entry, entry_error);
        }
    };

    auto build = [&]() -> Result<std::unique_ptr<Library>> {
        std::error_code create_error;
        std::filesystem::create_directories(directory, create_error);
        if (create_error)
            return fail(ErrorCode::AccessDenied,
                        std::format("cannot create library folder: {}", create_error.message()));
        for (const auto subdirectory : kLibrarySubdirectories) {
            std::filesystem::create_directories(directory / subdirectory, create_error);
            if (create_error)
                return fail(ErrorCode::AccessDenied,
                            std::format("cannot create {}: {}", subdirectory, create_error.message()));
        }
        std::unique_ptr<Library> library(new Library());
        library->directory_ = directory;
        library->catalog_file_ = directory / kCatalogFileName;
        library->uuid_ = new_uuid();
        library->readers_ = std::make_shared<detail::ReaderPool>(library->catalog_file_, OpenOptions{}.max_readers);
        if (auto initialized = library->initialize_new(name); !initialized) return std::unexpected(initialized.error());
        const LibraryManifest manifest{library->uuid_, std::string(name), kLibraryFormatVersion};
        if (auto written = write_manifest(directory / kManifestFileName, manifest); !written)
            return std::unexpected(written.error());
        return library;
    };

    auto library = build();  // destroyed (closing the database) before any cleanup below
    if (!library) remove_created();
    return library;
}

Result<void> Library::initialize_new(std::string_view name) {
    if (!sqlite::compiled_with("ENABLE_FTS5"))
        return fail(ErrorCode::Internal, "this SQLite build lacks FTS5; the pinned deps/sqlite build is required");
    auto connection = sqlite::Connection::open(catalog_file_, {.create = true});
    if (!connection) return std::unexpected(connection.error());
    writer_ = std::move(*connection);

    // One transaction for the whole schema: a crash leaves either nothing or a complete catalog.
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    for (const auto& migration : migrations()) {
        if (migration.version == 1) {
            if (auto applied = writer_.execute_script(migration.sql); !applied) return applied;
            auto insert = writer_.prepare(
                "INSERT INTO library_state(singleton,library_uuid,name,schema_version,catalog_revision,created_at,"
                "normalization_version) VALUES(1,?1,?2,?3,0,?4,?5)");
            if (!insert) return std::unexpected(insert.error());
            insert->bind(1, uuids::to_string(uuid_)).bind(2, name).bind(3, migration.version).bind(4, now_unix_ms());
            insert->bind(5, kFoldVersion);
            if (auto inserted = insert->run(); !inserted) return inserted;
            auto record =
                writer_.prepare("INSERT INTO schema_migrations(version,checksum,applied_at) VALUES(?1,?2,?3)");
            if (!record) return std::unexpected(record.error());
            record->bind(1, migration.version).bind(2, migration.checksum).bind(3, now_unix_ms());
            if (auto recorded = record->run(); !recorded) return recorded;
            continue;
        }
        if (auto applied = apply_migration_body(writer_, migration); !applied) return applied;
    }
    if (auto version = writer_.execute_script(std::format("PRAGMA user_version={}", latest_schema_version())); !version)
        return version;
    if (auto committed = transaction.commit(); !committed) return committed;
    if (auto tracking = detail::prepare_change_tracking(writer_); !tracking) return tracking;
    return load_cached_identity();
}

Result<std::unique_ptr<Library>> Library::open(const std::filesystem::path& directory, const OpenOptions& options) {
    auto manifest = read_manifest(directory / kManifestFileName);
    if (!manifest) return std::unexpected(manifest.error());
    if (manifest->format_version > kLibraryFormatVersion)
        return fail(ErrorCode::SchemaTooNew, "library.json was written by a newer version of the application");

    std::unique_ptr<Library> library(new Library());
    library->directory_ = directory;
    library->catalog_file_ = directory / kCatalogFileName;
    library->uuid_ = manifest->uuid;
    library->read_only_ = options.read_only;
    library->readers_ = std::make_shared<detail::ReaderPool>(library->catalog_file_, options.max_readers);

    auto connection = sqlite::Connection::open(library->catalog_file_, {.read_only = options.read_only});
    if (!connection) return std::unexpected(connection.error());
    library->writer_ = std::move(*connection);

    if (auto verified = library->verify_and_migrate(); !verified) return std::unexpected(verified.error());
    if (!library->read_only_) {
        if (auto tracking = detail::prepare_change_tracking(library->writer_); !tracking)
            return std::unexpected(tracking.error());
        if (auto recovered = library->recover_after_crash(); !recovered) return std::unexpected(recovered.error());
        if (auto rekeyed = library->refresh_normalized_keys(); !rekeyed) return std::unexpected(rekeyed.error());
    }
    if (auto identity = library->load_cached_identity(); !identity) return std::unexpected(identity.error());
    return library;
}

Result<void> Library::verify_and_migrate() {
    auto version = applied_schema_version(writer_);
    if (!version) return std::unexpected(version.error());
    if (*version == 0) return fail(ErrorCode::DbCorrupt, "catalog.sqlite has no schema");
    if (auto checksums = verify_migration_checksums(writer_); !checksums) return checksums;

    auto statement = writer_.prepare("SELECT library_uuid FROM library_state WHERE singleton=1");
    if (!statement) return std::unexpected(statement.error());
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    const bool matches = *has_row && statement->column_text(0) == uuids::to_string(uuid_);
    statement->reset();
    if (!matches) return fail(ErrorCode::DbCorrupt, "library.json and catalog.sqlite belong to different libraries");

    if (*version > latest_schema_version()) {
        read_only_ = true;  // SCHEMA_TOO_NEW: browse only, never write a newer catalog
        return {};
    }
    if (*version == latest_schema_version()) return {};
    if (read_only_)
        return fail(ErrorCode::InvalidArgument,
                    std::format("this library uses catalog schema {} and must be opened writable once to upgrade to {}",
                                *version, latest_schema_version()));

    auto backup = create_backup(std::format("pre-migration-v{}", *version));
    if (!backup) return std::unexpected(backup.error());
    recovery_.pre_migration_backup = *backup;
    for (const auto& migration : migrations()) {
        if (migration.version <= *version) continue;
        if (auto applied = apply_migration(writer_, migration); !applied) return applied;
    }
    auto integrity = writer_.integrity_check();
    if (!integrity) return std::unexpected(integrity.error());
    if (*integrity != "ok") return fail(ErrorCode::DbCorrupt, std::format("post-migration integrity: {}", *integrity));
    recovery_.migrated = true;
    return {};
}

Result<void> Library::recover_after_crash() {
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;

    if (auto paused = writer_.execute_script("UPDATE scan_sessions SET state='paused' WHERE state='running'"); !paused)
        return paused;
    recovery_.interrupted_scans = writer_.changed_row_count();

    if (auto partial = writer_.execute_script(
            "UPDATE history_actions SET state='partial' WHERE state IN ('applying','undoing','redoing')");
        !partial)
        return partial;
    recovery_.partial_history_actions = writer_.changed_row_count();
    // A chunked edit interrupted mid-way: its receipt says so, and a retry resumes or reports it.
    if (auto receipts = writer_.execute_script("UPDATE command_receipts SET state='partial' WHERE state='running'");
        !receipts)
        return receipts;

    // Background analysis stays paused after an unclean shutdown until the user resumes it.
    if (auto interrupted = writer_.execute_script(
            "UPDATE jobs SET state='interrupted', lease_token=NULL, lease_expires_at=NULL WHERE state='running'");
        !interrupted)
        return interrupted;
    recovery_.interrupted_jobs = writer_.changed_row_count();
    if (auto runs = writer_.execute_script(
            "UPDATE analysis_runs SET state='paused' WHERE state IN ('running','pausing','queued') "
            "AND EXISTS(SELECT 1 FROM jobs WHERE jobs.run_id=analysis_runs.id AND jobs.state='interrupted')");
        !runs)
        return runs;
    // Jobs of paused runs wait as `paused` (out of the jobs_ready lease index) until Resume.
    if (auto paused_jobs =
            writer_.execute_script("UPDATE jobs SET state='paused' WHERE state IN ('queued','interrupted') "
                                   "AND run_id IN (SELECT id FROM analysis_runs WHERE state='paused')");
        !paused_jobs)
        return paused_jobs;
    if (auto claims = writer_.execute_script("DELETE FROM analysis_claims"); !claims) return claims;

    if (auto committed = transaction.commit(); !committed) return committed;
    sweep_staging(directory_ / kStagingDirectory);
    return {};
}

Result<void> Library::refresh_normalized_keys() {
    const std::lock_guard lock(writer_mutex_);
    auto statement = writer_.prepare("SELECT normalization_version FROM library_state WHERE singleton=1");
    if (!statement) return std::unexpected(statement.error());
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::DbCorrupt, "library_state row is missing");
    const std::string stored_version = statement->column_text(0);
    statement->reset();
    if (stored_version == kFoldVersion) return {};

    // Keys under a unique index move in two passes: changing rows first get a unique placeholder
    // ('\x01' and the row id, which no folded name or path produces), so swapping two keys never
    // trips the index; a collision in the second pass is real.
    constexpr std::array<std::string_view, 12> kRekeyStatements = {
        "UPDATE samples SET name_key=fold_key(display_name) WHERE name_key IS NOT fold_key(display_name)",
        "UPDATE categories SET name_key=char(1)||id WHERE name_key IS NOT fold_key(name)",
        "UPDATE categories SET name_key=fold_key(name) WHERE name_key IS NOT fold_key(name)",
        "UPDATE tags SET name_key=char(1)||id WHERE name_key IS NOT fold_key(name)",
        "UPDATE tags SET name_key=fold_key(name) WHERE name_key IS NOT fold_key(name)",
        "UPDATE folders SET path_key=char(1)||id WHERE path_key IS NOT "
        "path_key(relative_path,(SELECT case_sensitive FROM roots WHERE roots.id=folders.root_id))",
        "UPDATE folders SET path_key=path_key(relative_path,(SELECT case_sensitive FROM roots "
        "WHERE roots.id=folders.root_id)) WHERE substr(path_key,1,1)=char(1)",
        "UPDATE locations SET path_key=char(1)||id WHERE path_key IS NOT "
        "path_key(relative_path,(SELECT case_sensitive FROM roots WHERE roots.id=locations.root_id))",
        "UPDATE locations SET path_key=path_key(relative_path,(SELECT case_sensitive FROM roots "
        "WHERE roots.id=locations.root_id)) WHERE substr(path_key,1,1)=char(1)",
        "UPDATE scan_frontier SET directory_key=path_key(relative_directory,(SELECT root.case_sensitive FROM "
        "scan_sessions session JOIN roots root ON root.id=session.root_id WHERE session.id=scan_frontier.scan_id)) "
        "WHERE directory_key IS NOT NULL",
        "UPDATE review_items SET state='resolved' WHERE kind='normalization_conflict' AND state='open'",
        "UPDATE library_state SET normalization_version=?1 WHERE singleton=1",
    };
    std::optional<Error> collision;
    {
        sqlite::WriteTransaction transaction(writer_);
        if (auto began = transaction.begin(); !began) return began;
        for (const auto sql : kRekeyStatements) {
            auto rekey = writer_.prepare(sql);
            if (!rekey) return std::unexpected(rekey.error());
            if (sql.find("?1") != std::string_view::npos) rekey->bind(1, kFoldVersion);
            if (auto ran = rekey->run(); !ran) {
                if (!rekey->failed_on_unique_constraint()) return ran;
                collision = ran.error();
                break;
            }
        }
        if (!collision) return transaction.commit();
    }  // the collided attempt rolls back here: the old keys stay consistent with each other

    // Two names or paths that the stored keys kept apart would share a key now. Nothing is merged
    // silently: the old keys stay (the next open tries again) and one open review item says why.
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    auto existing =
        writer_.query_int64("SELECT count(*) FROM review_items WHERE kind='normalization_conflict' AND state='open'");
    if (!existing) return std::unexpected(existing.error());
    if (*existing == 0)
        if (auto reviewed = detail::add_review_item(
                writer_, "normalization_conflict", std::nullopt,
                nlohmann::json{
                    {"stored_version", stored_version}, {"version", kFoldVersion}, {"detail", collision->safe_detail}});
            !reviewed)
            return reviewed;
    return transaction.commit();
}

Result<void> Library::load_cached_identity() {
    const std::lock_guard lock(writer_mutex_);
    auto statement =
        writer_.prepare("SELECT name, schema_version, catalog_revision FROM library_state WHERE singleton=1");
    if (!statement) return std::unexpected(statement.error());
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::DbCorrupt, "library_state row is missing");
    name_ = statement->column_text(0);
    schema_version_ = static_cast<int>(statement->column_int64(1));
    published_revision_.store(statement->column_int64(2));
    statement->reset();
    return {};
}

Library::~Library() {
    if (readers_) readers_->close();
}

LibraryIdentity Library::identity() const {
    return LibraryIdentity{uuid_, name_, schema_version_, published_revision_.load()};
}

Result<std::int64_t> Library::catalog_revision() {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto revision = reader->connection().query_int64("SELECT catalog_revision FROM library_state WHERE singleton=1");
    if (!revision) return std::unexpected(revision.error());
    publish_revision(published_revision_, *revision);
    return *revision;
}

Result<void> Library::ensure_writable() const {
    if (read_only_) return fail(ErrorCode::SchemaTooNew, "this library is open read-only");
    return {};
}

Result<void> Library::commit_revision(sqlite::WriteTransaction& transaction, std::int64_t revision) {
    if (auto committed = transaction.commit(); !committed) return committed;
    publish_revision(published_revision_, revision);
    return {};
}

// ---------------------------------------------------------------- reader pool

Result<ReaderLease> Library::acquire_reader(std::chrono::milliseconds timeout) {
    auto connection = readers_->acquire(timeout);
    if (!connection) return std::unexpected(connection.error());
    return ReaderLease(readers_, std::move(*connection));
}

// ---------------------------------------------------------------- maintenance

Result<std::filesystem::path> Library::create_backup(std::string_view reason) {
    const auto directory = directory_ / kBackupDirectory;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return fail(ErrorCode::AccessDenied, std::format("cannot create backups folder: {}", error.message()));
    const auto file = directory / std::format("{:013}-{}-{}.sqlite", now_unix_ms(), sanitized_backup_reason(reason),
                                              uuids::to_string(new_uuid()).substr(0, 8));
    {
        const std::lock_guard lock(writer_mutex_);
        if (auto copied = writer_.backup_to(file); !copied) {
            std::filesystem::remove(file, error);
            return std::unexpected(copied.error());
        }
    }
    if (auto verified = verify_backup(file, uuid_); !verified) {
        std::filesystem::remove(file, error);
        return std::unexpected(verified.error());
    }
    prune_backups(directory, kMaximumBackups);
    return file;
}

Result<std::string> Library::integrity_report() {
    // The whole-database checks run on a reader snapshot, so imports and edits keep writing meanwhile
    // instead of waiting on the writer mutex.
    std::string integrity;
    std::size_t violation_count = 0;
    {
        auto reader = acquire_reader();
        if (!reader) return std::unexpected(reader.error());
        auto& connection = reader->connection();
        sqlite::ReadTransaction snapshot(connection);
        if (auto began = snapshot.begin(); !began) return std::unexpected(began.error());
        auto checked = connection.integrity_check();
        if (!checked) return std::unexpected(checked.error());
        integrity = std::move(*checked);
        auto violations = connection.foreign_key_violations();
        if (!violations) return std::unexpected(violations.error());
        violation_count = violations->size();
    }
    // PRAGMA integrity_check does not compare an external-content FTS5 index with its content
    // table; FTS5's own 'integrity-check' command does, but it is an INSERT and so needs the writer.
    const std::lock_guard lock(writer_mutex_);
    auto fts = writer_.execute_script("INSERT INTO sample_fts(sample_fts, rank) VALUES('integrity-check', 1)");
    return std::format("integrity: {}; foreign keys: {} violation(s); search index: {}", integrity, violation_count,
                       fts ? "ok" : fts.error().safe_detail);
}

Result<void> Library::rebuild_search_index() {
    if (auto writable = ensure_writable(); !writable) return writable;
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    if (auto rebuilt = writer_.execute_script("INSERT INTO sample_fts(sample_fts) VALUES('rebuild')"); !rebuilt)
        return rebuilt;
    auto projected = writer_.query_int64("SELECT count(*) FROM sample_search");
    auto live = writer_.query_int64("SELECT count(*) FROM samples WHERE deleted=0");
    if (!projected) return std::unexpected(projected.error());
    if (!live) return std::unexpected(live.error());
    if (*projected != *live)
        return fail(ErrorCode::DbCorrupt,
                    std::format("search projection has {} rows for {} live samples", *projected, *live));
    if (auto checked = writer_.execute_script("INSERT INTO sample_fts(sample_fts, rank) VALUES('integrity-check', 1)");
        !checked)
        return checked;
    return transaction.commit();
}

}  // namespace mso::catalog
