// Roots, scan sessions and reconciliation of scanned files with existing locations.
#include <format>
#include <map>

#include "mso/catalog/catalog_internal.hpp"
#include "mso/catalog/library.hpp"

namespace mso::catalog {

namespace {

/// Last path segment of a '/'-separated relative path.
std::string_view file_name_of(std::string_view relative_path) {
    const auto slash = relative_path.rfind('/');
    return slash == std::string_view::npos ? relative_path : relative_path.substr(slash + 1);
}

/// Parent directory of a '/'-separated relative path ("" for files directly under the root).
std::string_view directory_of(std::string_view relative_path) {
    const auto slash = relative_path.rfind('/');
    return slash == std::string_view::npos ? std::string_view() : relative_path.substr(0, slash);
}

/// File name without its extension, used as the default display name.
std::string stem_of(std::string_view file_name) {
    const auto dot = file_name.rfind('.');
    return std::string(dot == std::string_view::npos || dot == 0 ? file_name : file_name.substr(0, dot));
}

nlohmann::json key_json(const MusicalKey& key) {
    return nlohmann::json{{"pitch_class", key.pitch_class}, {"mode", std::string(to_string(key.mode))}};
}

/// Applies one scan batch inside the caller's write transaction.
class ScanBatchWriter {
public:
    ScanBatchWriter(sqlite::Connection& connection, const ScanSession& session,
                    const Library::PathExistsProbe& path_exists)
        : connection_(connection), session_(session), path_exists_(path_exists) {}

    Result<ScanBatchOutcome> write(std::span<const ScannedFile> files) {
        for (const auto& file : files) {
            if (!is_safe_relative_path(file.relative_path) || !is_valid_utf8(file.relative_path))
                continue;  // never persist an escaping or undecodable path
            if (auto handled = write_file(file); !handled) return std::unexpected(handled.error());
        }
        if (auto projected = detail::refresh_search_projection(connection_, touched_samples_); !projected)
            return std::unexpected(projected.error());
        return outcome_;
    }

    const std::set<std::int64_t>& touched_samples() const { return touched_samples_; }

private:
    /// Existing live location at the same root-relative path.
    struct ExistingLocation {
        std::int64_t id = 0;
        std::int64_t sample_id = 0;
        std::optional<std::int64_t> byte_size;
        std::optional<std::int64_t> mtime_ns;
        std::optional<std::string> file_id;
    };

    Result<void> write_file(const ScannedFile& file) {
        const std::string path_key = fold_path_key(file.relative_path);
        auto existing = find_location_by_path(path_key);
        if (!existing) return std::unexpected(existing.error());
        if (*existing) return update_existing(**existing, file);

        auto relinked = try_relink(file, path_key);
        if (!relinked) return std::unexpected(relinked.error());
        if (*relinked) return {};
        return insert_new_sample(file, path_key);
    }

    Result<std::optional<ExistingLocation>> find_location_by_path(const std::string& path_key) {
        auto statement = connection_.prepare(
            "SELECT id, sample_id, byte_size, mtime_ns, file_id FROM locations "
            "WHERE root_id=?1 AND path_key=?2 AND state<>'retired'");
        if (!statement) return std::unexpected(statement.error());
        statement->bind(1, session_.root_id).bind(2, path_key);
        auto has_row = statement->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) return std::optional<ExistingLocation>{};
        return std::optional<ExistingLocation>(ExistingLocation{
            statement->column_int64(0), statement->column_int64(1), statement->column_optional_int64(2),
            statement->column_optional_int64(3), file_id_text(*statement, 4)});
    }

    static std::optional<std::string> file_id_text(sqlite::PreparedStatement& statement, int column) {
        if (statement.column_is_null(column)) return std::nullopt;
        const auto bytes = statement.column_blob(column);
        return std::string(bytes.begin(), bytes.end());
    }

    /// Same path seen again: unchanged, or content possibly changed (revision bump + invalidation).
    Result<void> update_existing(const ExistingLocation& existing, const ScannedFile& file) {
        const bool identity_changed = existing.file_id && file.file_id && *existing.file_id != *file.file_id;
        const bool stat_changed = existing.byte_size != file.byte_size || existing.mtime_ns != file.mtime_ns;
        if (auto updated = write_location_observation(existing.id, file, std::nullopt); !updated) return updated;
        if (!identity_changed && !stat_changed) {
            ++outcome_.unchanged;
            return {};
        }
        if (auto changed = record_content_change(existing.sample_id, file, identity_changed); !changed) return changed;
        ++outcome_.updated;
        return {};
    }

    /// A same-volume rename: exactly one location with this volume + file ID whose old path is gone.
    Result<bool> try_relink(const ScannedFile& file, const std::string& path_key) {
        if (!file.file_id || !file.volume_id) return false;
        auto statement = connection_.prepare(
            "SELECT id, sample_id, relative_path, byte_size, mtime_ns FROM locations "
            "WHERE root_id=?1 AND observed_volume_id=?2 AND file_id=?3 AND state<>'retired' AND path_key<>?4");
        if (!statement) return std::unexpected(statement.error());
        statement->bind(1, session_.root_id).bind(2, *file.volume_id);
        statement->bind_blob(3, file.file_id->data(), file.file_id->size()).bind(4, path_key);

        struct Candidate {
            std::int64_t location_id, sample_id;
            std::string old_path;
            std::optional<std::int64_t> byte_size, mtime_ns;
        };
        std::vector<Candidate> candidates;
        auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
            candidates.push_back({row.column_int64(0), row.column_int64(1), row.column_text(2),
                                  row.column_optional_int64(3), row.column_optional_int64(4)});
            return {};
        });
        if (!walked) return std::unexpected(walked.error());
        // File IDs can be reused after delete/recreate, so the old path must be gone and the
        // match unambiguous; anything else becomes a new sample (and maybe a review item).
        std::erase_if(candidates, [&](const Candidate& candidate) { return path_exists_(candidate.old_path); });
        if (candidates.size() != 1) {
            if (candidates.size() > 1)
                if (auto reviewed = detail::add_review_item(
                        connection_, "ambiguous_relink", std::nullopt,
                        nlohmann::json{{"new_path", file.relative_path}, {"candidates", candidates.size()}});
                    !reviewed)
                    return std::unexpected(reviewed.error());
            return false;
        }
        const auto& match = candidates.front();
        auto folder_id = ensure_folder(directory_of(file.relative_path));
        if (!folder_id) return std::unexpected(folder_id.error());
        auto move = connection_.prepare("UPDATE locations SET relative_path=?2, path_key=?3, folder_id=?4 WHERE id=?1");
        if (!move) return std::unexpected(move.error());
        move->bind(1, match.location_id).bind(2, file.relative_path).bind(3, path_key).bind(4, *folder_id);
        if (auto moved = move->run(); !moved) return std::unexpected(moved.error());
        if (auto observed = write_location_observation(match.location_id, file, std::nullopt); !observed)
            return std::unexpected(observed.error());
        if (match.byte_size != file.byte_size || match.mtime_ns != file.mtime_ns)
            if (auto changed = record_content_change(match.sample_id, file, false); !changed)
                return std::unexpected(changed.error());
        touched_samples_.insert(match.sample_id);
        ++outcome_.relinked;
        return true;
    }

    /// Records stat/identity for a location seen in this scan generation.
    Result<void> write_location_observation(std::int64_t location_id, const ScannedFile& file,
                                            std::optional<std::int64_t> content_revision) {
        auto update = connection_.prepare(
            "UPDATE locations SET byte_size=?2, mtime_ns=?3, observed_volume_id=?4, file_id=?5, state='present', "
            "seen_scan_generation=?6, last_seen_at=?7, observed_content_revision=COALESCE(?8, "
            "observed_content_revision) "
            "WHERE id=?1");
        if (!update) return std::unexpected(update.error());
        update->bind(1, location_id).bind(2, file.byte_size).bind(3, file.mtime_ns).bind(4, file.volume_id);
        if (file.file_id)
            update->bind_blob(5, file.file_id->data(), file.file_id->size());
        else
            update->bind_null(5);
        update->bind(6, session_.generation).bind(7, now_unix_ms()).bind(8, content_revision);
        return update->run();
    }

    /// Bytes may have changed: bump the content revision, keep user annotations, invalidate
    /// content-dependent results and re-derive technical facts from the new probe.
    Result<void> record_content_change(std::int64_t sample_id, const ScannedFile& file, bool identity_changed) {
        auto bump = connection_.prepare(
            "UPDATE samples SET content_revision=content_revision+1, updated_at=?2 WHERE id=?1 RETURNING "
            "content_revision");
        if (!bump) return std::unexpected(bump.error());
        bump->bind(1, sample_id).bind(2, now_unix_ms());
        auto has_row = bump->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        const std::int64_t revision = bump->column_int64(0);
        bump->reset();

        if (auto stale = connection_.execute_script(
                std::format("UPDATE artifacts SET state='stale' WHERE sample_id={0} AND state='ready';"
                            "UPDATE locations SET observed_content_revision={1}, byte_hash=NULL, hash_algorithm=NULL "
                            "WHERE sample_id={0} AND state<>'retired';",
                            sample_id, revision));
            !stale)
            return stale;
        if (auto technical = write_effective_metadata(sample_id, revision, file, /*insert=*/false); !technical)
            return technical;
        if (auto observed = write_observations(sample_id, revision, file); !observed) return observed;
        if (auto recomputed = detail::recompute_effective_musical_facts(connection_, sample_id); !recomputed)
            return recomputed;
        touched_samples_.insert(sample_id);
        return detail::add_review_item(
            connection_, "content_changed", sample_id,
            nlohmann::json{
                {"path", file.relative_path}, {"identity_changed", identity_changed}, {"content_revision", revision}});
    }

    Result<void> insert_new_sample(const ScannedFile& file, const std::string& path_key) {
        const std::string display_name = stem_of(file_name_of(file.relative_path));
        const auto now = now_unix_ms();
        auto insert = connection_.prepare(
            "INSERT INTO samples(uuid,display_name,name_key,created_at,updated_at) VALUES(?1,?2,?3,?4,?4)");
        if (!insert) return std::unexpected(insert.error());
        insert->bind(1, uuids::to_string(new_uuid()))
            .bind(2, display_name)
            .bind(3, fold_key(display_name))
            .bind(4, now);
        if (auto inserted = insert->run(); !inserted) return inserted;
        const std::int64_t sample_id = connection_.last_insert_rowid();

        auto folder_id = ensure_folder(directory_of(file.relative_path));
        if (!folder_id) return std::unexpected(folder_id.error());
        auto location = connection_.prepare(
            "INSERT INTO locations(sample_id,root_id,folder_id,relative_path,path_key,state,observed_content_revision) "
            "VALUES(?1,?2,?3,?4,?5,'present',1)");
        if (!location) return std::unexpected(location.error());
        location->bind(1, sample_id).bind(2, session_.root_id).bind(3, *folder_id).bind(4, file.relative_path);
        location->bind(5, path_key);
        if (auto inserted = location->run(); !inserted) return inserted;
        if (auto observed = write_location_observation(connection_.last_insert_rowid(), file, 1); !observed)
            return observed;

        if (auto technical = write_effective_metadata(sample_id, 1, file, /*insert=*/true); !technical)
            return technical;
        if (auto observed = write_observations(sample_id, 1, file); !observed) return observed;
        if (auto recomputed = detail::recompute_effective_musical_facts(connection_, sample_id); !recomputed)
            return recomputed;
        if (auto weak = note_possible_move(sample_id, file); !weak) return weak;
        touched_samples_.insert(sample_id);
        ++outcome_.added;
        return {};
    }

    /// A missing location with the same file name and size suggests a move the identity check
    /// could not prove (e.g. across volumes): propose it for review, never reassign automatically.
    Result<void> note_possible_move(std::int64_t new_sample_id, const ScannedFile& file) {
        auto statement = connection_.prepare(
            "SELECT sample_id, relative_path FROM locations WHERE state='missing' AND byte_size=?1 "
            "AND (relative_path=?2 OR relative_path LIKE '%/' || ?2) LIMIT 4");
        if (!statement) return std::unexpected(statement.error());
        statement->bind(1, file.byte_size).bind(2, std::string(file_name_of(file.relative_path)));
        nlohmann::json candidates = nlohmann::json::array();
        auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
            candidates.push_back({{"sample_id", row.column_int64(0)}, {"old_path", row.column_text(1)}});
            return {};
        });
        if (!walked) return walked;
        if (candidates.empty()) return {};
        return detail::add_review_item(connection_, "possible_move", new_sample_id,
                                       nlohmann::json{{"new_path", file.relative_path}, {"candidates", candidates}});
    }

    Result<void> write_effective_metadata(std::int64_t sample_id, std::int64_t revision, const ScannedFile& file,
                                          bool insert) {
        const ObservedMetadata empty;
        const ObservedMetadata& metadata = file.metadata ? *file.metadata : empty;
        const std::string sql =
            insert
                ? "INSERT INTO effective_metadata(sample_id,content_revision,format,codec,duration_us,duration_exact,"
                  "source_frames,sample_rate,channels,bit_depth,byte_size) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)"
                : "UPDATE effective_metadata SET content_revision=?2, format=?3, codec=?4, duration_us=?5, "
                  "duration_exact=?6, source_frames=?7, sample_rate=?8, channels=?9, bit_depth=?10, byte_size=?11, "
                  "analysis_flags=0 WHERE sample_id=?1";
        auto statement = connection_.prepare(sql);
        if (!statement) return std::unexpected(statement.error());
        auto optional_text = [](const std::string& text) {
            return text.empty() ? std::optional<std::string>() : std::optional<std::string>(text);
        };
        auto positive = [](const std::optional<std::int64_t>& value) {
            return value && *value > 0 ? value : std::optional<std::int64_t>();
        };
        statement->bind(1, sample_id).bind(2, revision).bind(3, optional_text(metadata.format));
        statement->bind(4, optional_text(metadata.codec));
        statement->bind(5, metadata.duration_us && *metadata.duration_us >= 0 ? metadata.duration_us : std::nullopt);
        statement->bind(6, metadata.duration_exact ? 1 : 0);
        statement->bind(7, metadata.frames && *metadata.frames >= 0 ? metadata.frames : std::nullopt);
        statement->bind(8, positive(metadata.sample_rate)).bind(9, positive(metadata.channels));
        statement->bind(10, positive(metadata.bit_depth)).bind(11, file.byte_size);
        return statement->run();
    }

    /// Provenanced facts: container tags and filename heuristics, for this content revision.
    Result<void> write_observations(std::int64_t sample_id, std::int64_t revision, const ScannedFile& file) {
        if (!file.metadata) return {};
        const auto& metadata = *file.metadata;
        auto insert = connection_.prepare(
            "INSERT INTO metadata_observations(sample_id,content_revision,field,value_json,source,created_at) "
            "VALUES(?1,?2,?3,?4,?5,?6)");
        if (!insert) return std::unexpected(insert.error());
        auto observe = [&](std::string_view field, const nlohmann::json& value,
                           std::string_view source) -> Result<void> {
            insert->bind(1, sample_id).bind(2, revision).bind(3, field).bind(4, value.dump()).bind(5, source);
            insert->bind(6, now_unix_ms());
            return insert->run();
        };
        auto valid_bpm = [](std::int64_t milli) { return milli >= 20'000 && milli <= 400'000; };
        if (metadata.embedded_bpm_milli && valid_bpm(*metadata.embedded_bpm_milli))
            if (auto done = observe("bpm_milli", *metadata.embedded_bpm_milli, "container"); !done) return done;
        if (metadata.embedded_key)
            if (auto done = observe("key", key_json(*metadata.embedded_key), "container"); !done) return done;
        if (metadata.filename_bpm_milli && valid_bpm(*metadata.filename_bpm_milli))
            if (auto done = observe("bpm_milli", *metadata.filename_bpm_milli, "filename"); !done) return done;
        if (metadata.filename_key)
            if (auto done = observe("key", key_json(*metadata.filename_key), "filename"); !done) return done;
        if (!metadata.embedded_text.empty())
            if (auto done = observe("embedded_text", metadata.embedded_text, "container"); !done) return done;
        if (metadata.loop_count > 0)
            if (auto done = observe("loop_count", metadata.loop_count, "container"); !done) return done;
        return {};
    }

    /// Ensures folder rows exist for a directory and all its ancestors; returns the leaf id.
    Result<std::optional<std::int64_t>> ensure_folder(std::string_view relative_directory) {
        if (relative_directory.empty()) return std::optional<std::int64_t>{};
        const std::string key = fold_path_key(relative_directory);
        if (const auto cached = folder_cache_.find(key); cached != folder_cache_.end())
            return std::optional<std::int64_t>(cached->second);

        auto parent = ensure_folder(directory_of(relative_directory));
        if (!parent) return std::unexpected(parent.error());
        auto insert = connection_.prepare(
            "INSERT INTO folders(root_id,parent_id,relative_path,path_key) VALUES(?1,?2,?3,?4) "
            "ON CONFLICT(root_id,path_key) DO UPDATE SET relative_path=excluded.relative_path RETURNING id");
        if (!insert) return std::unexpected(insert.error());
        insert->bind(1, session_.root_id).bind(2, *parent).bind(3, relative_directory).bind(4, key);
        auto has_row = insert->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        const std::int64_t id = insert->column_int64(0);
        insert->reset();
        folder_cache_.emplace(key, id);
        return std::optional<std::int64_t>(id);
    }

    sqlite::Connection& connection_;
    const ScanSession& session_;
    const Library::PathExistsProbe& path_exists_;
    ScanBatchOutcome outcome_;
    std::set<std::int64_t> touched_samples_;
    std::map<std::string, std::int64_t> folder_cache_;
};

}  // namespace

// ---------------------------------------------------------------- roots

Result<RootRecord> Library::add_root(std::string_view name, RootMode mode) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    if (trim(name).empty()) return fail(ErrorCode::InvalidArgument, "a root needs a name");
    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    RootRecord root;
    root.uuid = new_uuid();
    root.name = trim(name);
    root.mode = mode;
    auto insert =
        writer_.prepare("INSERT INTO roots(uuid,name,mode,relative_base,case_sensitive) VALUES(?1,?2,?3,?4,0)");
    if (!insert) return std::unexpected(insert.error());
    insert->bind(1, uuids::to_string(root.uuid)).bind(2, root.name).bind(3, to_string(mode));
    if (mode == RootMode::Managed)
        insert->bind(4, std::format("media/{}", uuids::to_string(root.uuid)));
    else
        insert->bind_null(4);
    if (auto inserted = insert->run(); !inserted) return std::unexpected(inserted.error());
    root.id = writer_.last_insert_rowid();
    if (auto revision = detail::advance_revision(
            writer_, nlohmann::json{{"kind", "root_added"}, {"root", uuids::to_string(root.uuid)}});
        !revision)
        return std::unexpected(revision.error());
    if (auto committed = transaction.commit(); !committed) return std::unexpected(committed.error());
    return root;
}

Result<std::vector<RootRecord>> Library::list_roots() {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto statement = reader->connection().prepare(
        "SELECT r.id, r.uuid, r.name, r.mode, r.availability, r.scan_generation, r.last_seen_at, "
        "(SELECT count(DISTINCT l.sample_id) FROM locations l JOIN samples s ON s.id=l.sample_id "
        " WHERE l.root_id=r.id AND l.state<>'retired' AND s.deleted=0) "
        "FROM roots r WHERE r.deleted=0 ORDER BY r.name, r.id");
    if (!statement) return std::unexpected(statement.error());
    std::vector<RootRecord> roots;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        RootRecord root;
        root.id = row.column_int64(0);
        root.uuid = uuids::uuid::from_string(row.column_text(1)).value_or(uuids::uuid());
        root.name = row.column_text(2);
        root.mode = parse_root_mode(row.column_text(3)).value_or(RootMode::Referenced);
        root.availability = parse_availability(row.column_text(4)).value_or(Availability::Unknown);
        root.scan_generation = row.column_int64(5);
        root.last_seen_at = row.column_optional_int64(6);
        root.sample_count = row.column_int64(7);
        roots.push_back(std::move(root));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return roots;
}

Result<void> Library::set_root_availability(std::int64_t root_id, Availability availability) {
    if (auto writable = ensure_writable(); !writable) return writable;
    std::lock_guard lock(writer_mutex_);
    auto current = writer_.prepare("SELECT availability FROM roots WHERE id=?1");
    if (!current) return std::unexpected(current.error());
    current->bind(1, root_id);
    auto has_row = current->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::NotFound, "no such root");
    if (current->column_text(0) == to_string(availability)) return {};  // no revision for a no-op
    current->reset();

    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    auto update = writer_.prepare(
        "UPDATE roots SET availability=?2, last_seen_at=CASE WHEN ?2='online' THEN ?3 ELSE last_seen_at END WHERE "
        "id=?1");
    if (!update) return std::unexpected(update.error());
    update->bind(1, root_id).bind(2, to_string(availability)).bind(3, now_unix_ms());
    if (auto updated = update->run(); !updated) return updated;
    if (auto revision = detail::advance_revision(writer_, nlohmann::json{{"kind", "root_availability"},
                                                                         {"root_id", root_id},
                                                                         {"availability", to_string(availability)}});
        !revision)
        return std::unexpected(revision.error());
    return transaction.commit();
}

Result<void> Library::rename_root(std::int64_t root_id, std::string_view name) {
    if (auto writable = ensure_writable(); !writable) return writable;
    if (trim(name).empty()) return fail(ErrorCode::InvalidArgument, "a root needs a name");
    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    auto update = writer_.prepare("UPDATE roots SET name=?2 WHERE id=?1");
    if (!update) return std::unexpected(update.error());
    update->bind(1, root_id).bind(2, trim(name));
    if (auto updated = update->run(); !updated) return updated;
    // Location display paths include the root name.
    auto members = writer_.prepare("SELECT DISTINCT sample_id FROM locations WHERE root_id=?1");
    if (!members) return std::unexpected(members.error());
    members->bind(1, root_id);
    std::set<std::int64_t> samples;
    auto walked = detail::for_each_row(*members, [&](sqlite::PreparedStatement& row) -> Result<void> {
        samples.insert(row.column_int64(0));
        return {};
    });
    if (!walked) return walked;
    if (auto projected = detail::refresh_search_projection(writer_, samples); !projected) return projected;
    if (auto revision =
            detail::advance_revision(writer_, nlohmann::json{{"kind", "root_renamed"}, {"root_id", root_id}});
        !revision)
        return std::unexpected(revision.error());
    return transaction.commit();
}

// ---------------------------------------------------------------- scans

Result<ScanSession> Library::begin_scan(std::int64_t root_id) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());

    // Resume an interrupted scan of this root so its completed directories are not redone.
    auto paused = writer_.prepare(
        "SELECT id, generation FROM scan_sessions WHERE root_id=?1 AND state IN ('paused','running') "
        "ORDER BY generation DESC LIMIT 1");
    if (!paused) return std::unexpected(paused.error());
    paused->bind(1, root_id);
    auto has_row = paused->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    ScanSession session{.root_id = root_id};
    if (*has_row) {
        session.id = paused->column_int64(0);
        session.generation = paused->column_int64(1);
        paused->reset();
        auto resume = writer_.prepare("UPDATE scan_sessions SET state='running' WHERE id=?1");
        if (!resume) return std::unexpected(resume.error());
        resume->bind(1, session.id);
        if (auto resumed = resume->run(); !resumed) return std::unexpected(resumed.error());
    } else {
        paused->reset();
        auto bump = writer_.prepare(
            "UPDATE roots SET scan_generation=scan_generation+1 WHERE id=?1 AND deleted=0 RETURNING scan_generation");
        if (!bump) return std::unexpected(bump.error());
        bump->bind(1, root_id);
        auto bumped = bump->next_row();
        if (!bumped) return std::unexpected(bumped.error());
        if (!*bumped) return fail(ErrorCode::NotFound, "no such root");
        session.generation = bump->column_int64(0);
        bump->reset();
        auto insert = writer_.prepare(
            "INSERT INTO scan_sessions(root_id,generation,state,started_at) VALUES(?1,?2,'running',?3)");
        if (!insert) return std::unexpected(insert.error());
        insert->bind(1, root_id).bind(2, session.generation).bind(3, now_unix_ms());
        if (auto inserted = insert->run(); !inserted) return std::unexpected(inserted.error());
        session.id = writer_.last_insert_rowid();
    }
    if (auto committed = transaction.commit(); !committed) return std::unexpected(committed.error());
    return session;
}

Result<std::vector<std::string>> Library::completed_directories(const ScanSession& session) {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto statement = reader->connection().prepare(
        "SELECT relative_directory FROM scan_frontier WHERE scan_id=?1 AND state='complete'");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, session.id);
    std::vector<std::string> directories;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        directories.push_back(row.column_text(0));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return directories;
}

Result<ScanBatchOutcome> Library::commit_scan_batch(const ScanSession& session, std::span<const ScannedFile> files,
                                                    std::span<const std::string> completed,
                                                    const PathExistsProbe& path_exists) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());

    ScanBatchWriter batch(writer_, session, path_exists);
    auto outcome = batch.write(files);
    if (!outcome) return std::unexpected(outcome.error());

    auto frontier = writer_.prepare(
        "INSERT INTO scan_frontier(scan_id,relative_directory,state) VALUES(?1,?2,'complete') "
        "ON CONFLICT(scan_id,relative_directory) DO UPDATE SET state='complete'");
    if (!frontier) return std::unexpected(frontier.error());
    for (const auto& directory : completed) {
        frontier->bind(1, session.id).bind(2, directory);
        if (auto marked = frontier->run(); !marked) return std::unexpected(marked.error());
    }

    nlohmann::json samples = nlohmann::json::array();
    for (const auto id : batch.touched_samples())
        samples.push_back(id);
    if (auto revision = detail::advance_revision(
            writer_, nlohmann::json{{"kind", "scan_batch"}, {"root_id", session.root_id}, {"samples", samples}});
        !revision)
        return std::unexpected(revision.error());
    if (auto committed = transaction.commit(); !committed) return std::unexpected(committed.error());
    return outcome;
}

Result<void> Library::record_scan_gap(const ScanSession& session, std::string_view relative_directory, bool denied) {
    if (auto writable = ensure_writable(); !writable) return writable;
    std::lock_guard lock(writer_mutex_);
    auto statement = writer_.prepare(
        "INSERT INTO scan_frontier(scan_id,relative_directory,state) VALUES(?1,?2,?3) "
        "ON CONFLICT(scan_id,relative_directory) DO UPDATE SET state=excluded.state");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, session.id).bind(2, relative_directory).bind(3, denied ? "denied" : "failed");
    return statement->run();
}

Result<ScanFinishOutcome> Library::finish_scan(const ScanSession& session, bool cancelled) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    ScanFinishOutcome outcome;

    if (cancelled) {
        // Already committed batches stay; the frontier remains so the next scan resumes.
        auto pause = writer_.prepare("UPDATE scan_sessions SET state='paused' WHERE id=?1");
        if (!pause) return std::unexpected(pause.error());
        pause->bind(1, session.id);
        if (auto paused = pause->run(); !paused) return std::unexpected(paused.error());
    } else {
        auto gaps = writer_.query_int64(std::format(
            "SELECT count(*) FROM scan_frontier WHERE scan_id={} AND state IN ('denied','failed')", session.id));
        if (!gaps) return std::unexpected(gaps.error());
        outcome.complete_coverage = *gaps == 0;

        // Unseen locations become missing, except inside subtrees that could not be enumerated:
        // those are unknown, not empty.
        auto mark = writer_.prepare(
            "UPDATE locations SET state='missing' WHERE root_id=?1 AND state IN ('present','changed','unknown') "
            "AND seen_scan_generation<?2 AND NOT EXISTS (SELECT 1 FROM scan_frontier f WHERE f.scan_id=?3 "
            "AND f.state IN ('denied','failed') AND (f.relative_directory='' OR "
            "locations.relative_path LIKE f.relative_directory || '/%'))");
        if (!mark) return std::unexpected(mark.error());
        mark->bind(1, session.root_id).bind(2, session.generation).bind(3, session.id);
        if (auto marked = mark->run(); !marked) return std::unexpected(marked.error());
        outcome.marked_missing = writer_.changed_row_count();

        auto complete = writer_.prepare("UPDATE scan_sessions SET state='completed', complete_coverage=?2 WHERE id=?1");
        if (!complete) return std::unexpected(complete.error());
        complete->bind(1, session.id).bind(2, outcome.complete_coverage ? 1 : 0);
        if (auto completed = complete->run(); !completed) return std::unexpected(completed.error());
        auto seen = writer_.prepare("UPDATE roots SET availability='online', last_seen_at=?2 WHERE id=?1");
        if (!seen) return std::unexpected(seen.error());
        seen->bind(1, session.root_id).bind(2, now_unix_ms());
        if (auto updated = seen->run(); !updated) return std::unexpected(updated.error());
    }
    if (auto revision = detail::advance_revision(writer_, nlohmann::json{{"kind", "scan_finished"},
                                                                         {"root_id", session.root_id},
                                                                         {"cancelled", cancelled},
                                                                         {"marked_missing", outcome.marked_missing}});
        !revision)
        return std::unexpected(revision.error());
    if (auto committed = transaction.commit(); !committed) return std::unexpected(committed.error());
    return outcome;
}

}  // namespace mso::catalog

namespace mso::catalog {

Result<std::map<std::string, Library::KnownStat>> Library::known_stats_in_directory(
    std::int64_t root_id, std::string_view relative_directory) {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto statement = reader->connection().prepare(
        relative_directory.empty()
            ? "SELECT path_key, byte_size, mtime_ns FROM locations WHERE root_id=?1 AND folder_id IS NULL "
              "AND state<>'retired'"
            : "SELECT l.path_key, l.byte_size, l.mtime_ns FROM locations l JOIN folders f ON f.id=l.folder_id "
              "WHERE l.root_id=?1 AND f.root_id=?1 AND f.path_key=?2 AND l.state<>'retired'");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, root_id);
    if (!relative_directory.empty()) statement->bind(2, fold_path_key(relative_directory));
    std::map<std::string, KnownStat> stats;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        stats.emplace(row.column_text(0), KnownStat{row.column_optional_int64(1), row.column_optional_int64(2)});
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return stats;
}

}  // namespace mso::catalog
