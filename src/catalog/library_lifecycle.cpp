// Library creation, opening, migrations, crash recovery, reader pool and backups.
#include <format>
#include <fstream>
#include <sstream>

#include "mso/catalog/catalog_internal.hpp"
#include "mso/catalog/library.hpp"
#include "mso/catalog/migrations.hpp"

namespace mso::catalog {

namespace {

constexpr std::string_view kBackupDirectory = "backups";
constexpr std::string_view kStagingDirectory = "staging";

/// The identity manifest stored next to the database.
struct LibraryManifest {
    uuids::uuid uuid;
    std::string name;
    int format_version = 0;
};

Result<void> write_file_atomically(const std::filesystem::path& target, std::string_view contents) {
    auto temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return fail(ErrorCode::AccessDenied, std::format("cannot write {}", path_to_utf8(target)));
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out) return fail(ErrorCode::DiskFull, std::format("write failed for {}", path_to_utf8(target)));
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error)
        return fail(ErrorCode::AccessDenied,
                    std::format("cannot replace {}: {}", path_to_utf8(target), error.message()));
    return {};
}

Result<LibraryManifest> read_manifest(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail(ErrorCode::NotFound, std::format("no library manifest at {}", path_to_utf8(file)));
    std::stringstream contents;
    contents << in.rdbuf();
    auto document = nlohmann::json::parse(contents.str(), nullptr, false);
    if (document.is_discarded() || !document.is_object())
        return fail(ErrorCode::DbCorrupt, "library.json is not valid JSON");
    LibraryManifest manifest;
    const auto uuid_text = document.value("library_uuid", std::string());
    auto uuid = uuids::uuid::from_string(uuid_text);
    if (!uuid) return fail(ErrorCode::DbCorrupt, "library.json has no valid library_uuid");
    manifest.uuid = *uuid;
    manifest.name = document.value("name", std::string());
    manifest.format_version = document.value("format_version", 0);
    return manifest;
}

Result<void> write_manifest(const std::filesystem::path& file, const LibraryManifest& manifest) {
    nlohmann::json document = {{"format_version", manifest.format_version},
                               {"library_uuid", uuids::to_string(manifest.uuid)},
                               {"name", manifest.name},
                               {"application", "Music Sample Organizer"}};
    return write_file_atomically(file, document.dump(2));
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

/// Released migrations must be byte-identical to what this build ships.
Result<void> verify_migration_checksums(sqlite::Connection& connection) {
    auto statement = connection.prepare("SELECT version, checksum FROM schema_migrations ORDER BY version");
    if (!statement) return std::unexpected(statement.error());
    return detail::for_each_row(*statement, [](sqlite::PreparedStatement& row) -> Result<void> {
        const auto version = static_cast<int>(row.column_int64(0));
        for (const auto& migration : migrations()) {
            if (migration.version == version && migration.checksum != row.column_text(1))
                return fail(ErrorCode::DbCorrupt,
                            std::format("migration {} checksum differs from this build", version));
        }
        return {};
    });
}

/// Applies one migration in its own transaction, recording its checksum.
Result<void> apply_migration(sqlite::Connection& connection, const Migration& migration) {
    sqlite::WriteTransaction transaction(connection);
    if (auto began = transaction.begin(); !began) return began;
    if (auto applied = connection.execute_script(migration.sql); !applied) return applied;
    auto record = connection.prepare("INSERT INTO schema_migrations(version,checksum,applied_at) VALUES(?1,?2,?3)");
    if (!record) return std::unexpected(record.error());
    record->bind(1, migration.version).bind(2, migration.checksum).bind(3, now_unix_ms());
    if (auto recorded = record->run(); !recorded) return recorded;
    if (auto has_state = connection.query_int64("SELECT count(*) FROM library_state"); has_state && *has_state > 0) {
        auto update = connection.prepare("UPDATE library_state SET schema_version=?1 WHERE singleton=1");
        if (!update) return std::unexpected(update.error());
        update->bind(1, migration.version);
        if (auto updated = update->run(); !updated) return updated;
    }
    if (auto version = connection.execute_script(std::format("PRAGMA user_version={}", migration.version)); !version)
        return version;
    return transaction.commit();
}

/// Removes temporary files left by interrupted operations in staging/ (only our own *.partial).
void sweep_staging(const std::filesystem::path& staging) {
    std::error_code error;
    if (!std::filesystem::is_directory(staging, error)) return;
    for (const auto& entry : std::filesystem::directory_iterator(staging, error)) {
        if (entry.is_regular_file(error) && entry.path().extension() == ".partial")
            std::filesystem::remove(entry.path(), error);
    }
}

}  // namespace

// ---------------------------------------------------------------- ReaderLease

ReaderLease::ReaderLease(Library* owner, std::unique_ptr<sqlite::Connection> connection)
    : owner_(owner), connection_(std::move(connection)) {}

ReaderLease::ReaderLease(ReaderLease&& other) noexcept
    : owner_(other.owner_), connection_(std::move(other.connection_)) {
    other.owner_ = nullptr;
}

ReaderLease::~ReaderLease() {
    if (owner_ && connection_) owner_->release_reader(std::move(connection_));
}

// ---------------------------------------------------------------- creation and opening

Result<std::unique_ptr<Library>> Library::create(const std::filesystem::path& directory, std::string_view name) {
    std::error_code error;
    if (std::filesystem::exists(directory, error) && !std::filesystem::is_empty(directory, error))
        return fail(ErrorCode::InvalidArgument, "the library folder must be new or empty");
    std::filesystem::create_directories(directory, error);
    if (error) return fail(ErrorCode::AccessDenied, std::format("cannot create library folder: {}", error.message()));
    for (auto sub : {"media", "derived/waveforms", "derived/features", "backups", "staging"})
        std::filesystem::create_directories(directory / sub, error);

    std::unique_ptr<Library> library(new Library());
    library->directory_ = directory;
    library->catalog_file_ = directory / kCatalogFileName;
    library->uuid_ = new_uuid();
    if (auto initialized = library->initialize_new(name); !initialized) return std::unexpected(initialized.error());

    const LibraryManifest manifest{library->uuid_, std::string(name), kLibraryFormatVersion};
    if (auto written = write_manifest(directory / kManifestFileName, manifest); !written)
        return std::unexpected(written.error());
    return library;
}

Result<void> Library::initialize_new(std::string_view name) {
    if (!sqlite::compiled_with("ENABLE_FTS5"))
        return fail(ErrorCode::Internal, "this SQLite build lacks FTS5; the pinned deps/sqlite build is required");
    auto connection = sqlite::Connection::open(catalog_file_, {.create = true});
    if (!connection) return std::unexpected(connection.error());
    writer_ = std::move(*connection);
    for (const auto& migration : migrations()) {
        if (auto applied = apply_migration(writer_, migration); !applied) return applied;
        if (migration.version == 1) {
            auto insert = writer_.prepare(
                "INSERT INTO library_state(singleton,library_uuid,name,schema_version,catalog_revision,created_at,"
                "normalization_version) VALUES(1,?1,?2,?3,0,?4,?5)");
            if (!insert) return std::unexpected(insert.error());
            insert->bind(1, uuids::to_string(uuid_)).bind(2, name).bind(3, migration.version).bind(4, now_unix_ms());
            insert->bind(5, kFoldVersion);
            if (auto inserted = insert->run(); !inserted) return inserted;
        }
    }
    auto update = writer_.prepare("UPDATE library_state SET schema_version=?1");
    if (!update) return std::unexpected(update.error());
    update->bind(1, latest_schema_version());
    return update->run();
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
    library->max_readers_ = std::max(1, options.max_readers);

    auto connection = sqlite::Connection::open(library->catalog_file_, {.read_only = options.read_only});
    if (!connection) return std::unexpected(connection.error());
    library->writer_ = std::move(*connection);

    if (auto verified = library->verify_and_migrate(); !verified) return std::unexpected(verified.error());
    if (!library->read_only_) {
        if (auto recovered = library->recover_after_crash(); !recovered) return std::unexpected(recovered.error());
    }
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
    if (!*has_row || statement->column_text(0) != uuids::to_string(uuid_))
        return fail(ErrorCode::DbCorrupt, "library.json and catalog.sqlite belong to different libraries");
    statement->reset();

    if (*version > latest_schema_version()) {
        read_only_ = true;  // SCHEMA_TOO_NEW: browse only, never write a newer catalog
        return {};
    }
    if (*version == latest_schema_version() || read_only_) return {};

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
    std::lock_guard lock(writer_mutex_);
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
    if (auto cleared = writer_.execute_script("UPDATE history_state SET active_action_id=NULL"); !cleared)
        return cleared;

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
    if (auto claims = writer_.execute_script("DELETE FROM analysis_claims"); !claims) return claims;

    if (auto committed = transaction.commit(); !committed) return committed;
    sweep_staging(directory_ / kStagingDirectory);
    return {};
}

Library::~Library() {
    std::lock_guard lock(readers_mutex_);
    idle_readers_.clear();
}

LibraryIdentity Library::identity() {
    LibraryIdentity identity;
    identity.uuid = uuid_;
    std::lock_guard lock(writer_mutex_);
    auto statement =
        writer_.prepare("SELECT name, schema_version, catalog_revision FROM library_state WHERE singleton=1");
    if (statement && statement->next_row().value_or(false)) {
        identity.name = statement->column_text(0);
        identity.schema_version = static_cast<int>(statement->column_int64(1));
        identity.catalog_revision = statement->column_int64(2);
    }
    return identity;
}

Result<void> Library::ensure_writable() const {
    if (read_only_) return fail(ErrorCode::SchemaTooNew, "this library is open read-only");
    return {};
}

// ---------------------------------------------------------------- reader pool

Result<ReaderLease> Library::acquire_reader() {
    std::unique_lock lock(readers_mutex_);
    reader_available_.wait(lock, [&] { return !idle_readers_.empty() || open_readers_ < max_readers_; });
    if (!idle_readers_.empty()) {
        auto connection = std::move(idle_readers_.back());
        idle_readers_.pop_back();
        return ReaderLease(this, std::move(connection));
    }
    ++open_readers_;
    lock.unlock();
    auto connection = sqlite::Connection::open(catalog_file_, {.read_only = true, .page_cache_kib = 8 * 1024});
    if (!connection) {
        std::lock_guard relock(readers_mutex_);
        --open_readers_;
        reader_available_.notify_one();
        return std::unexpected(connection.error());
    }
    return ReaderLease(this, std::make_unique<sqlite::Connection>(std::move(*connection)));
}

void Library::release_reader(std::unique_ptr<sqlite::Connection> connection) {
    std::lock_guard lock(readers_mutex_);
    connection->rollback();  // never return a connection with an open read transaction
    idle_readers_.push_back(std::move(connection));
    reader_available_.notify_one();
}

// ---------------------------------------------------------------- maintenance

Result<std::filesystem::path> Library::create_backup(std::string_view reason) {
    const auto directory = directory_ / kBackupDirectory;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto file = directory / std::format("{}-{}.sqlite", now_unix_ms(), reason);
    {
        std::lock_guard lock(writer_mutex_);
        if (auto copied = writer_.backup_to(file); !copied) {
            std::filesystem::remove(file, error);
            return std::unexpected(copied.error());
        }
    }
    if (auto verified = verify_backup(file, uuid_); !verified) {
        std::filesystem::remove(file, error);
        return std::unexpected(verified.error());
    }
    return file;
}

Result<std::string> Library::integrity_report() {
    std::lock_guard lock(writer_mutex_);
    auto integrity = writer_.integrity_check();
    if (!integrity) return std::unexpected(integrity.error());
    auto violations = writer_.foreign_key_violations();
    if (!violations) return std::unexpected(violations.error());
    auto fts = writer_.execute_script("INSERT INTO sample_fts(sample_fts, rank) VALUES('integrity-check', 1)");
    std::string report = std::format("integrity: {}; foreign keys: {} violation(s); search index: {}", *integrity,
                                     violations->size(), fts ? "ok" : fts.error().safe_detail);
    return report;
}

Result<void> Library::rebuild_search_index() {
    if (auto writable = ensure_writable(); !writable) return writable;
    std::lock_guard lock(writer_mutex_);
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
