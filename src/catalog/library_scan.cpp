// Roots, scan sessions and reconciliation of scanned files with existing locations.
#include <format>
#include <map>

#include "mso/catalog/detail/catalog_internal.hpp"
#include "mso/catalog/library.hpp"
#include "mso/catalog/root_paths.hpp"

namespace mso::catalog {

namespace {

constexpr std::int64_t kMinimumMilliBpm = 20'000;
constexpr std::int64_t kMaximumMilliBpm = 400'000;

/// Samples (as `sample_id`) with a non-retired location in root ?1.
constexpr std::string_view kRootSamplesSql =
    "SELECT DISTINCT location.sample_id AS sample_id FROM locations location "
    "WHERE location.root_id=?1 AND location.state<>'retired'";

/// Old paths of relink candidates, probed before the writer is taken: path → still exists on disk.
using ProbedPaths = std::map<std::string, bool, std::less<>>;

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

/// Files a scan may persist: safe, valid UTF-8 relative paths.
bool is_storable(const ScannedFile& file) {
    return is_safe_relative_path(file.relative_path) && is_valid_utf8(file.relative_path);
}

/// Whether a root distinguishes file names by case. NOT_FOUND for an unknown or deleted root.
Result<bool> root_case_sensitivity(sqlite::Connection& connection, std::int64_t root_id) {
    auto statement = connection.prepare("SELECT case_sensitive FROM roots WHERE id=?1 AND deleted=0");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, root_id);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::NotFound, "no such root");
    const bool case_sensitive = statement->column_int64(0) != 0;
    statement->reset();
    return case_sensitive;
}

/// Reads, on a reader connection, the old paths of every possible same-volume rename in the batch.
/// The caller probes them after returning the reader (probe_paths), so no filesystem probe runs while
/// a reader snapshot or the writer is held.
Result<std::vector<std::string>> relink_candidate_paths(sqlite::Connection& reader, std::int64_t root_id,
                                                        bool case_sensitive, std::span<const ScannedFile> files) {
    sqlite::ReadTransaction snapshot(reader);
    if (auto began = snapshot.begin(); !began) return std::unexpected(began.error());
    auto statement = reader.prepare(
        "SELECT relative_path FROM locations WHERE root_id=?1 AND observed_volume_id=?2 AND file_id=?3 "
        "AND state<>'retired' AND path_key<>?4");
    if (!statement) return std::unexpected(statement.error());
    std::vector<std::string> old_paths;
    for (const auto& file : files) {
        if (!file.file_id || !file.volume_id || !is_storable(file)) continue;
        statement->bind(1, root_id).bind(2, *file.volume_id).bind_blob(3, file.file_id->data(), file.file_id->size());
        statement->bind(4, mso::path_key(file.relative_path, case_sensitive));
        auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
            old_paths.push_back(row.column_text(0));
            return {};
        });  // for_each_row resets the statement, so the next file rebinds it
        if (!walked) return std::unexpected(walked.error());
    }
    return old_paths;
}

/// Asks `path_exists` once about each distinct old path.
ProbedPaths probe_paths(std::vector<std::string> old_paths, const Library::PathExistsProbe& path_exists) {
    ProbedPaths probed;
    for (auto& old_path : old_paths) {
        if (probed.contains(old_path)) continue;
        const bool exists = path_exists(old_path);
        probed.emplace(std::move(old_path), exists);
    }
    return probed;
}

/// Applies one scan batch inside the caller's write transaction.
class ScanBatchWriter {
public:
    ScanBatchWriter(sqlite::Connection& connection, const ScanSession& session, bool case_sensitive,
                    const ProbedPaths& probed_paths)
        : connection_(connection), session_(session), case_sensitive_(case_sensitive), probed_paths_(probed_paths) {}

    Result<ScanBatchOutcome> write(std::span<const ScannedFile> files) {
        for (const auto& file : files) {
            if (!is_storable(file)) {
                ++outcome_.rejected_paths;  // never persist an escaping or undecodable path
                continue;
            }
            if (auto handled = write_file(file); !handled) return std::unexpected(handled.error());
        }
        return outcome_;
    }

    /// True when folders were created or renamed, or samples added (navigation counts changed).
    bool structure_changed() const { return structure_changed_; }

private:
    /// Existing live location at the same root-relative path.
    struct ExistingLocation {
        std::int64_t id = 0;
        std::int64_t sample_id = 0;
        std::optional<std::int64_t> byte_size;
        std::optional<std::int64_t> mtime_ns;
        std::optional<std::string> file_id;
        std::string state;
        std::string relative_path;  ///< stored spelling (may differ in case on a case-insensitive root)
    };

    std::string key_of(std::string_view relative_path) const { return mso::path_key(relative_path, case_sensitive_); }

    Result<void> write_file(const ScannedFile& file) {
        const std::string path_key = key_of(file.relative_path);
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
            "SELECT id, sample_id, byte_size, mtime_ns, file_id, state, relative_path FROM locations "
            "WHERE root_id=?1 AND path_key=?2 AND state<>'retired'");
        if (!statement) return std::unexpected(statement.error());
        statement->bind(1, session_.root_id).bind(2, path_key);
        auto has_row = statement->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) return std::optional<ExistingLocation>{};
        ExistingLocation location{statement->column_int64(0),
                                  statement->column_int64(1),
                                  statement->column_optional_int64(2),
                                  statement->column_optional_int64(3),
                                  file_id_text(*statement, 4),
                                  statement->column_text(5),
                                  statement->column_text(6)};
        statement->reset();
        return std::optional<ExistingLocation>(std::move(location));
    }

    static std::optional<std::string> file_id_text(sqlite::PreparedStatement& statement, int column) {
        if (statement.column_is_null(column)) return std::nullopt;
        const auto bytes = statement.column_blob(column);
        return std::string(bytes.begin(), bytes.end());
    }

    /// Same path seen again: unchanged, or content possibly changed (revision bump + invalidation).
    /// On a case-insensitive root the same key may arrive in a new spelling (a case-only rename):
    /// the stored path and its folder follow the spelling on disk.
    Result<void> update_existing(const ExistingLocation& existing, const ScannedFile& file) {
        const bool identity_changed = existing.file_id && file.file_id && *existing.file_id != *file.file_id;
        const bool stat_changed = existing.byte_size != file.byte_size || existing.mtime_ns != file.mtime_ns;
        if (existing.relative_path != file.relative_path)
            if (auto respelled = respell_location(existing, file.relative_path); !respelled) return respelled;
        if (auto updated = write_location_observation(existing.id, file, std::nullopt); !updated) return updated;
        if (existing.state != "present")  // a missing/unknown file is back: its row's availability changed
            if (auto marked = detail::mark_sample_changed(connection_, existing.sample_id, false); !marked)
                return marked;
        if (!identity_changed && !stat_changed) {
            ++outcome_.unchanged;
            return {};
        }
        if (auto changed = record_content_change(existing.sample_id, file, identity_changed); !changed) return changed;
        ++outcome_.updated;
        return {};
    }

    /// Stores a case-only rename of a location on a case-insensitive root (same path key).
    Result<void> respell_location(const ExistingLocation& existing, const std::string& relative_path) {
        auto folder_id = ensure_folder(directory_of(relative_path));
        if (!folder_id) return std::unexpected(folder_id.error());
        auto respell = connection_.prepare("UPDATE locations SET relative_path=?2, folder_id=?3 WHERE id=?1");
        if (!respell) return std::unexpected(respell.error());
        respell->bind(1, existing.id).bind(2, relative_path).bind(3, *folder_id);
        if (auto updated = respell->run(); !updated) return updated;
        return detail::mark_sample_changed(connection_, existing.sample_id, /*search_text_changed=*/true);
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
        // File IDs can be reused after delete/recreate, so the old path must be gone and the match
        // unambiguous; anything else becomes a new sample (and maybe a review item). A candidate that
        // appeared after the pre-probe was not examined and counts as still existing.
        std::erase_if(candidates, [&](const Candidate& candidate) {
            const auto probed = probed_paths_.find(candidate.old_path);
            return probed == probed_paths_.end() || probed->second;
        });
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
        if (auto marked = detail::mark_sample_changed(connection_, match.sample_id, /*search_text_changed=*/true);
            !marked)
            return std::unexpected(marked.error());
        structure_changed_ = true;  // folder counts moved
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
        if (!*has_row) return fail(ErrorCode::NotFound, "sample not found");
        const std::int64_t revision = bump->column_int64(0);
        bump->reset();

        auto stale = connection_.prepare("UPDATE artifacts SET state='stale' WHERE sample_id=?1 AND state='ready'");
        if (!stale) return std::unexpected(stale.error());
        stale->bind(1, sample_id);
        if (auto staled = stale->run(); !staled) return staled;
        auto unhash = connection_.prepare(
            "UPDATE locations SET observed_content_revision=?2, byte_hash=NULL, hash_algorithm=NULL "
            "WHERE sample_id=?1 AND state<>'retired'");
        if (!unhash) return std::unexpected(unhash.error());
        unhash->bind(1, sample_id).bind(2, revision);
        if (auto unhashed = unhash->run(); !unhashed) return unhashed;

        if (auto technical = write_effective_metadata(sample_id, revision, file, /*insert=*/false); !technical)
            return technical;
        if (auto observed = write_observations(sample_id, revision, file); !observed) return observed;
        if (auto recomputed = detail::recompute_effective_musical_facts(connection_, sample_id); !recomputed)
            return recomputed;
        if (auto marked = detail::mark_sample_changed(connection_, sample_id, /*search_text_changed=*/true); !marked)
            return marked;
        return detail::add_review_item(
            connection_, "content_changed", sample_id,
            nlohmann::json{
                {"path", file.relative_path}, {"identity_changed", identity_changed}, {"content_revision", revision}});
    }

    Result<void> insert_new_sample(const ScannedFile& file, const std::string& path_key) {
        const std::string display_name = stem_of(file_name_of(file.relative_path));
        const auto now = now_unix_ms();
        auto insert = connection_.prepare(
            "INSERT INTO samples(uuid,display_name,name_key,created_at,updated_at) VALUES(new_uuid(),?1,?2,?3,?3)");
        if (!insert) return std::unexpected(insert.error());
        insert->bind(1, display_name).bind(2, fold_key(display_name)).bind(3, now);
        if (auto inserted = insert->run(); !inserted) return inserted;
        const std::int64_t sample_id = connection_.last_insert_rowid();

        auto folder_id = ensure_folder(directory_of(file.relative_path));
        if (!folder_id) return std::unexpected(folder_id.error());
        auto location = connection_.prepare(
            "INSERT INTO locations(sample_id,root_id,folder_id,relative_path,path_key,state,observed_content_revision,"
            "byte_size,mtime_ns,observed_volume_id,file_id,seen_scan_generation,last_seen_at) "
            "VALUES(?1,?2,?3,?4,?5,'present',1,?6,?7,?8,?9,?10,?11)");
        if (!location) return std::unexpected(location.error());
        location->bind(1, sample_id).bind(2, session_.root_id).bind(3, *folder_id).bind(4, file.relative_path);
        location->bind(5, path_key).bind(6, file.byte_size).bind(7, file.mtime_ns).bind(8, file.volume_id);
        if (file.file_id)
            location->bind_blob(9, file.file_id->data(), file.file_id->size());
        else
            location->bind_null(9);
        location->bind(10, session_.generation).bind(11, now);
        if (auto inserted = location->run(); !inserted) return inserted;

        if (auto technical = write_effective_metadata(sample_id, 1, file, /*insert=*/true); !technical)
            return technical;
        if (auto observed = write_observations(sample_id, 1, file); !observed) return observed;
        if (auto recomputed = detail::recompute_effective_musical_facts(connection_, sample_id); !recomputed)
            return recomputed;
        if (auto weak = note_possible_move(sample_id, file); !weak) return weak;
        if (auto marked = detail::mark_sample_changed(connection_, sample_id, /*search_text_changed=*/true); !marked)
            return marked;
        structure_changed_ = true;  // root, folder and tree counts grew
        ++outcome_.added;
        return {};
    }

    /// A missing location with the same file name and size suggests a move the identity check
    /// could not prove: propose it for review, never reassign automatically. Deliberately searched
    /// across all roots, because a move to another volume (another root) is exactly the case the
    /// identity check cannot prove. The name match is an exact suffix compare (no LIKE wildcards).
    Result<void> note_possible_move(std::int64_t new_sample_id, const ScannedFile& file) {
        auto statement = connection_.prepare(
            "SELECT sample_id, relative_path FROM locations WHERE state='missing' AND byte_size=?1 "
            "AND (relative_path=?2 OR (length(relative_path)>length(?2) "
            "AND substr(relative_path, -length(?2)-1)='/' || ?2)) LIMIT 4");
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
        auto statement = connection_.prepare(
            insert
                ? "INSERT INTO effective_metadata(sample_id,content_revision,format,codec,duration_us,duration_exact,"
                  "source_frames,sample_rate,channels,bit_depth,byte_size) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)"
                : "UPDATE effective_metadata SET content_revision=?2, format=?3, codec=?4, duration_us=?5, "
                  "duration_exact=?6, source_frames=?7, sample_rate=?8, channels=?9, bit_depth=?10, byte_size=?11, "
                  "analysis_flags=0 WHERE sample_id=?1");
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
        statement->bind(10, positive(metadata.bit_depth));
        statement->bind(11, file.byte_size >= 0 ? std::optional<std::int64_t>(file.byte_size) : std::nullopt);
        return statement->run();
    }

    /// Provenanced facts: container tags and filename heuristics, for this content revision. A failed
    /// probe is recorded as a `probe_error` observation so the inspector can say why facts are missing.
    Result<void> write_observations(std::int64_t sample_id, std::int64_t revision, const ScannedFile& file) {
        auto insert = connection_.prepare(
            "INSERT INTO metadata_observations(sample_id,content_revision,field,value_json,source,created_at) "
            "VALUES(?1,?2,?3,?4,?5,?6)");
        if (!insert) return std::unexpected(insert.error());
        auto observe = [&](std::string_view field, const nlohmann::json& value,
                           std::string_view source) -> Result<void> {
            insert->bind(1, sample_id).bind(2, revision).bind(3, field).bind(4, json_fields::dump_json(value));
            insert->bind(5, source).bind(6, now_unix_ms());
            return insert->run();
        };
        if (file.probe_error)
            if (auto done = observe("probe_error", std::string(to_string(*file.probe_error)), "container"); !done)
                return done;
        if (!file.metadata) return {};
        const auto& metadata = *file.metadata;
        auto valid_bpm = [](std::int64_t milli) { return milli >= kMinimumMilliBpm && milli <= kMaximumMilliBpm; };
        auto valid_key = [](const MusicalKey& key) { return key.pitch_class >= 0 && key.pitch_class <= 11; };
        if (metadata.embedded_bpm_milli && valid_bpm(*metadata.embedded_bpm_milli))
            if (auto done = observe("bpm_milli", *metadata.embedded_bpm_milli, "container"); !done) return done;
        if (metadata.embedded_key && valid_key(*metadata.embedded_key))
            if (auto done = observe("key", key_json(*metadata.embedded_key), "container"); !done) return done;
        if (metadata.filename_bpm_milli && valid_bpm(*metadata.filename_bpm_milli))
            if (auto done = observe("bpm_milli", *metadata.filename_bpm_milli, "filename"); !done) return done;
        if (metadata.filename_key && valid_key(*metadata.filename_key))
            if (auto done = observe("key", key_json(*metadata.filename_key), "filename"); !done) return done;
        if (!metadata.embedded_text.empty())
            if (auto done = observe("embedded_text", metadata.embedded_text, "container"); !done) return done;
        if (metadata.loop_count > 0)
            if (auto done = observe("loop_count", metadata.loop_count, "container"); !done) return done;
        return {};
    }

    /// Ensures folder rows exist for a directory and all its ancestors; returns the leaf id (nullopt
    /// for the root itself). Iterative: walks up to the nearest known ancestor, then creates or
    /// respells downwards, so a very deep tree cannot exhaust the importer thread's stack.
    Result<std::optional<std::int64_t>> ensure_folder(std::string_view relative_directory) {
        struct PendingFolder {
            std::string_view relative_directory;
            std::string key;
        };
        std::vector<PendingFolder> pending;  // leaf first
        std::optional<std::int64_t> parent;
        for (auto directory = relative_directory; !directory.empty(); directory = directory_of(directory)) {
            std::string key = key_of(directory);
            if (const auto cached = folder_cache_.find(key); cached != folder_cache_.end()) {
                parent = cached->second;
                break;
            }
            pending.push_back({directory, std::move(key)});
        }
        for (auto folder = pending.rbegin(); folder != pending.rend(); ++folder) {
            auto id = find_or_create_folder(folder->relative_directory, folder->key, parent);
            if (!id) return std::unexpected(id.error());
            folder_cache_.emplace(std::move(folder->key), *id);
            parent = *id;
        }
        return parent;
    }

    /// The folder row of one directory whose parent row is `parent_id`: found (and respelled when
    /// its case changed on a case-insensitive root) or created.
    Result<std::int64_t> find_or_create_folder(std::string_view relative_directory, const std::string& key,
                                               std::optional<std::int64_t> parent_id) {
        auto find = connection_.prepare("SELECT id, relative_path FROM folders WHERE root_id=?1 AND path_key=?2");
        if (!find) return std::unexpected(find.error());
        find->bind(1, session_.root_id).bind(2, key);
        auto found = find->next_row();
        if (!found) return std::unexpected(found.error());
        if (*found) {
            const std::int64_t id = find->column_int64(0);
            const bool respelled = find->column_text(1) != relative_directory;  // case change on a folded root
            find->reset();
            if (respelled) {
                auto rename = connection_.prepare("UPDATE folders SET relative_path=?2 WHERE id=?1");
                if (!rename) return std::unexpected(rename.error());
                rename->bind(1, id).bind(2, relative_directory);
                if (auto renamed = rename->run(); !renamed) return std::unexpected(renamed.error());
                structure_changed_ = true;
            }
            return id;
        }
        find->reset();
        auto insert =
            connection_.prepare("INSERT INTO folders(root_id,parent_id,relative_path,path_key) VALUES(?1,?2,?3,?4)");
        if (!insert) return std::unexpected(insert.error());
        insert->bind(1, session_.root_id).bind(2, parent_id).bind(3, relative_directory).bind(4, key);
        if (auto inserted = insert->run(); !inserted) return std::unexpected(inserted.error());
        structure_changed_ = true;
        return connection_.last_insert_rowid();
    }

    sqlite::Connection& connection_;
    const ScanSession& session_;
    const bool case_sensitive_;
    const ProbedPaths& probed_paths_;
    ScanBatchOutcome outcome_;
    bool structure_changed_ = false;
    std::map<std::string, std::int64_t> folder_cache_;
};

}  // namespace

// ---------------------------------------------------------------- roots

Result<RootRecord> Library::add_root(std::string_view name, RootMode mode, std::optional<bool> case_sensitive) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    const std::string trimmed = trim(name);
    if (trimmed.empty() || !is_valid_utf8(trimmed)) return fail(ErrorCode::InvalidArgument, "a root needs a name");
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    RootRecord root;
    root.uuid = new_uuid();
    root.name = trimmed;
    root.mode = mode;
    root.case_sensitive = case_sensitive.value_or(default_case_sensitivity());
    auto insert =
        writer_.prepare("INSERT INTO roots(uuid,name,mode,relative_base,case_sensitive) VALUES(?1,?2,?3,?4,?5)");
    if (!insert) return std::unexpected(insert.error());
    insert->bind(1, uuids::to_string(root.uuid)).bind(2, root.name).bind(3, to_string(mode));
    if (mode == RootMode::Managed)
        insert->bind(4, std::format("media/{}", uuids::to_string(root.uuid)));
    else
        insert->bind_null(4);
    insert->bind(5, root.case_sensitive ? 1 : 0);
    if (auto inserted = insert->run(); !inserted) return std::unexpected(inserted.error());
    root.id = writer_.last_insert_rowid();
    auto revision =
        detail::advance_revision(writer_, {.kind = "root_added",
                                           .structure_changed = true,
                                           .details = {{"root", uuids::to_string(root.uuid)}, {"root_id", root.id}}});
    if (!revision) return std::unexpected(revision.error());
    if (auto committed = commit_revision(transaction, *revision); !committed) return std::unexpected(committed.error());
    return root;
}

Result<std::vector<RootRecord>> Library::list_roots() {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto statement = reader->connection().prepare(
        "SELECT root.id, root.uuid, root.name, root.mode, root.availability, root.scan_generation, root.last_seen_at, "
        "(SELECT count(DISTINCT location.sample_id) FROM locations location JOIN samples sample "
        " ON sample.id=location.sample_id WHERE location.root_id=root.id AND location.state<>'retired' "
        " AND sample.deleted=0), root.case_sensitive "
        "FROM roots root WHERE root.deleted=0 ORDER BY root.name, root.id");
    if (!statement) return std::unexpected(statement.error());
    std::vector<RootRecord> roots;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        RootRecord root;
        root.id = row.column_int64(0);
        auto uuid = detail::parse_stored_uuid(row.column_text(1), "root");
        if (!uuid) return std::unexpected(uuid.error());
        root.uuid = *uuid;
        root.name = row.column_text(2);
        const auto mode = parse_root_mode(row.column_text(3));
        const auto availability = parse_availability(row.column_text(4));
        if (!mode || !availability) return fail(ErrorCode::DbCorrupt, "root has an unknown mode or availability");
        root.mode = *mode;
        root.availability = *availability;
        root.scan_generation = row.column_int64(5);
        root.last_seen_at = row.column_optional_int64(6);
        root.sample_count = row.column_int64(7);
        root.case_sensitive = row.column_int64(8) != 0;
        roots.push_back(std::move(root));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return roots;
}

Result<void> Library::set_root_availability(std::int64_t root_id, Availability availability) {
    if (auto writable = ensure_writable(); !writable) return writable;
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    auto current = writer_.prepare("SELECT availability FROM roots WHERE id=?1 AND deleted=0");
    if (!current) return std::unexpected(current.error());
    current->bind(1, root_id);
    auto has_row = current->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::NotFound, "no such root");
    const bool unchanged = current->column_text(0) == to_string(availability);
    current->reset();
    if (unchanged) return {};  // no revision for a no-op; the transaction rolls back

    auto update = writer_.prepare(
        "UPDATE roots SET availability=?2, last_seen_at=CASE WHEN ?2='online' THEN ?3 ELSE last_seen_at END WHERE "
        "id=?1");
    if (!update) return std::unexpected(update.error());
    update->bind(1, root_id).bind(2, to_string(availability)).bind(3, now_unix_ms());
    if (auto updated = update->run(); !updated) return updated;
    const std::int64_t parameters[] = {root_id};
    if (auto marked = detail::mark_samples_changed(writer_, kRootSamplesSql, parameters, false); !marked) return marked;
    if (auto blocked = detail::block_unreachable_jobs(writer_, root_id); !blocked) return blocked;
    if (auto unblocked = detail::unblock_reachable_jobs(writer_, root_id); !unblocked) return unblocked;
    auto revision = detail::advance_revision(
        writer_, {.kind = "root_availability",
                  .structure_changed = true,
                  .details = {{"root_id", root_id}, {"availability", to_string(availability)}}});
    if (!revision) return std::unexpected(revision.error());
    return commit_revision(transaction, *revision);
}

Result<void> Library::rename_root(std::int64_t root_id, std::string_view name) {
    if (auto writable = ensure_writable(); !writable) return writable;
    const std::string trimmed = trim(name);
    if (trimmed.empty() || !is_valid_utf8(trimmed)) return fail(ErrorCode::InvalidArgument, "a root needs a name");
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    auto update = writer_.prepare("UPDATE roots SET name=?2 WHERE id=?1 AND deleted=0");
    if (!update) return std::unexpected(update.error());
    update->bind(1, root_id).bind(2, trimmed);
    if (auto updated = update->run(); !updated) return updated;
    if (writer_.changed_row_count() == 0) return fail(ErrorCode::NotFound, "no such root");
    // Location display paths include the root name (set-based projection refresh below).
    const std::int64_t parameters[] = {root_id};
    if (auto marked = detail::mark_samples_changed(writer_, kRootSamplesSql, parameters, true); !marked) return marked;
    auto revision = detail::advance_revision(
        writer_, {.kind = "root_renamed", .structure_changed = true, .details = {{"root_id", root_id}}});
    if (!revision) return std::unexpected(revision.error());
    return commit_revision(transaction, *revision);
}

// ---------------------------------------------------------------- scans

Result<ScanSession> Library::begin_scan(std::int64_t root_id) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    if (auto exists = root_case_sensitivity(writer_, root_id); !exists) return std::unexpected(exists.error());

    // An interrupted scan that is too old to trust is abandoned: the directories it completed may
    // have changed since, so a fresh scan visits everything again.
    auto abandon = writer_.prepare(
        "UPDATE scan_sessions SET state='cancelled' WHERE root_id=?1 AND state IN ('paused','running') "
        "AND started_at<?2");
    if (!abandon) return std::unexpected(abandon.error());
    abandon->bind(1, root_id).bind(2, now_unix_ms() - std::chrono::milliseconds(kMaximumScanResumeAge).count());
    if (auto abandoned = abandon->run(); !abandoned) return std::unexpected(abandoned.error());

    // Resume a recently interrupted scan of this root so its completed directories are not redone.
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
    if (files.size() > kMaximumScanBatchFiles)
        return fail(ErrorCode::InvalidArgument,
                    std::format("a scan batch holds at most {} files (got {})", kMaximumScanBatchFiles, files.size()));

    // Old paths of possible renames are read on a reader, then probed on disk with neither the
    // reader nor the writer held.
    std::vector<std::string> old_paths;
    bool case_sensitive = false;
    {
        auto reader = acquire_reader();
        if (!reader) return std::unexpected(reader.error());
        auto sensitivity = root_case_sensitivity(reader->connection(), session.root_id);
        if (!sensitivity) return std::unexpected(sensitivity.error());
        case_sensitive = *sensitivity;
        auto candidates = relink_candidate_paths(reader->connection(), session.root_id, case_sensitive, files);
        if (!candidates) return std::unexpected(candidates.error());
        old_paths = std::move(*candidates);
    }
    const ProbedPaths probed_paths = probe_paths(std::move(old_paths), path_exists);

    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());

    ScanBatchWriter batch(writer_, session, case_sensitive, probed_paths);
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

    auto revision = detail::advance_revision(writer_, {.kind = "scan_batch",
                                                       .structure_changed = batch.structure_changed(),
                                                       .details = {{"root_id", session.root_id}}});
    if (!revision) return std::unexpected(revision.error());
    if (auto committed = commit_revision(transaction, *revision); !committed) return std::unexpected(committed.error());
    return outcome;
}

Result<void> Library::mark_scan_entries_seen(const ScanSession& session, std::span<const std::string> relative_paths) {
    if (auto writable = ensure_writable(); !writable) return writable;
    if (relative_paths.size() > kMaximumScanBatchFiles)
        return fail(ErrorCode::InvalidArgument, std::format("at most {} skipped entries per call (got {})",
                                                            kMaximumScanBatchFiles, relative_paths.size()));
    if (relative_paths.empty()) return {};
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    auto case_sensitive = root_case_sensitivity(writer_, session.root_id);
    if (!case_sensitive) return std::unexpected(case_sensitive.error());

    // Only the scan bookkeeping changes (stats stay as they were). A location that an earlier scan
    // marked missing exists again, but nothing about its bytes was verified: it becomes `unknown`.
    auto revive = writer_.prepare(
        "UPDATE locations SET state='unknown', seen_scan_generation=?3, last_seen_at=?4 "
        "WHERE root_id=?1 AND path_key=?2 AND state='missing' RETURNING sample_id");
    auto mark = writer_.prepare(
        "UPDATE locations SET seen_scan_generation=?3, last_seen_at=?4 "
        "WHERE root_id=?1 AND path_key=?2 AND state<>'retired'");
    if (!revive) return std::unexpected(revive.error());
    if (!mark) return std::unexpected(mark.error());
    std::vector<std::int64_t> revived_samples;
    const auto now = now_unix_ms();
    for (const auto& relative_path : relative_paths) {
        const std::string path_key = mso::path_key(relative_path, *case_sensitive);
        revive->bind(1, session.root_id).bind(2, path_key).bind(3, session.generation).bind(4, now);
        auto walked = detail::for_each_row(*revive, [&](sqlite::PreparedStatement& row) -> Result<void> {
            revived_samples.push_back(row.column_int64(0));
            return {};
        });
        if (!walked) return walked;
        mark->bind(1, session.root_id).bind(2, path_key).bind(3, session.generation).bind(4, now);
        if (auto marked = mark->run(); !marked) return marked;
    }
    if (revived_samples.empty()) return transaction.commit();  // bookkeeping only: no catalog revision

    if (auto marked = detail::mark_sample_list_changed(writer_, revived_samples, false); !marked) return marked;
    auto revision = detail::advance_revision(
        writer_, {.kind = "scan_seen", .structure_changed = true, .details = {{"root_id", session.root_id}}});
    if (!revision) return std::unexpected(revision.error());
    return commit_revision(transaction, *revision);
}

Result<void> Library::record_scan_gap(const ScanSession& session, std::string_view relative_directory, bool denied) {
    if (auto writable = ensure_writable(); !writable) return writable;
    const std::lock_guard lock(writer_mutex_);
    auto case_sensitive = root_case_sensitivity(writer_, session.root_id);
    if (!case_sensitive) return std::unexpected(case_sensitive.error());
    // The key lets finish_scan match the subtree by path key, so on a case-insensitive root a gap
    // spelled in another case still protects the locations below it.
    auto statement = writer_.prepare(
        "INSERT INTO scan_frontier(scan_id,relative_directory,state,directory_key) VALUES(?1,?2,?3,?4) "
        "ON CONFLICT(scan_id,relative_directory) DO UPDATE SET state=excluded.state, "
        "directory_key=excluded.directory_key");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, session.id).bind(2, relative_directory).bind(3, denied ? "denied" : "failed");
    statement->bind(4, mso::path_key(relative_directory, *case_sensitive));
    return statement->run();
}

Result<ScanFinishOutcome> Library::finish_scan(const ScanSession& session, bool cancelled) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    ScanFinishOutcome outcome;
    bool structure_changed = false;

    if (cancelled) {
        // Already committed batches stay; the frontier remains so the next scan resumes.
        auto pause = writer_.prepare("UPDATE scan_sessions SET state='paused' WHERE id=?1");
        if (!pause) return std::unexpected(pause.error());
        pause->bind(1, session.id);
        if (auto paused = pause->run(); !paused) return std::unexpected(paused.error());
    } else {
        auto gaps = writer_.prepare(
            "SELECT count(*) FROM scan_frontier INDEXED BY scan_frontier_state WHERE scan_id=?1 "
            "AND state IN ('denied','failed')");
        if (!gaps) return std::unexpected(gaps.error());
        gaps->bind(1, session.id);
        auto has_gaps = gaps->next_row();
        if (!has_gaps) return std::unexpected(has_gaps.error());
        outcome.complete_coverage = *has_gaps && gaps->column_int64(0) == 0;
        gaps->reset();

        // Unseen locations become missing, except inside subtrees that could not be enumerated:
        // those are unknown, not empty. Prefixes are compared exactly on path keys (no LIKE: '_' and
        // '%' are common in sample names); gap rows from before migration 6 have no key and compare
        // their spelling. Without gaps the subquery is left out, and with gaps it finds them through
        // scan_frontier_state instead of reading every frontier row per unseen location.
        constexpr std::string_view kUnseenCondition =
            "location.root_id=?1 AND location.state IN ('present','changed','unknown') "
            "AND location.seen_scan_generation<?2";
        constexpr std::string_view kOutsideGapsCondition =
            " AND NOT EXISTS (SELECT 1 FROM scan_frontier gap INDEXED BY scan_frontier_state "
            "WHERE gap.scan_id=?3 AND gap.state IN ('denied','failed') AND (gap.relative_directory='' OR "
            "(gap.directory_key IS NOT NULL AND substr(location.path_key, 1, length(gap.directory_key)+1)="
            "gap.directory_key || '/') OR (gap.directory_key IS NULL AND "
            "substr(location.relative_path, 1, length(gap.relative_directory)+1)=gap.relative_directory || '/')))";
        const std::string condition = outcome.complete_coverage
                                          ? std::string(kUnseenCondition)
                                          : std::format("{}{}", kUnseenCondition, kOutsideGapsCondition);
        const std::int64_t all_parameters[] = {session.root_id, session.generation, session.id};
        const auto parameters = std::span<const std::int64_t>(all_parameters).first(outcome.complete_coverage ? 2 : 3);
        if (auto marked = detail::mark_samples_changed(
                writer_,
                std::format("SELECT location.sample_id AS sample_id FROM locations location WHERE {}", condition),
                parameters, false);
            !marked)
            return std::unexpected(marked.error());
        auto mark =
            writer_.prepare(std::format("UPDATE locations AS location SET state='missing' WHERE {}", condition));
        if (!mark) return std::unexpected(mark.error());
        for (std::size_t index = 0; index < parameters.size(); ++index)
            mark->bind(static_cast<int>(index) + 1, parameters[index]);
        if (auto marked = mark->run(); !marked) return std::unexpected(marked.error());
        outcome.marked_missing = writer_.changed_row_count();
        structure_changed = outcome.marked_missing > 0;

        // Folders left without any location (every file moved away by a rename) and without
        // subfolders are removed, innermost first; a folder that still holds missing files stays.
        auto prune = writer_.prepare(
            "DELETE FROM folders WHERE root_id=?1 "
            "AND NOT EXISTS(SELECT 1 FROM locations location WHERE location.folder_id=folders.id) "
            "AND NOT EXISTS(SELECT 1 FROM folders child WHERE child.root_id=?1 AND child.parent_id=folders.id)");
        if (!prune) return std::unexpected(prune.error());
        while (true) {
            prune->bind(1, session.root_id);
            if (auto pruned = prune->run(); !pruned) return std::unexpected(pruned.error());
            if (writer_.changed_row_count() == 0) break;
            outcome.removed_empty_folders += writer_.changed_row_count();
            structure_changed = true;
        }

        auto complete = writer_.prepare("UPDATE scan_sessions SET state='completed', complete_coverage=?2 WHERE id=?1");
        if (!complete) return std::unexpected(complete.error());
        complete->bind(1, session.id).bind(2, outcome.complete_coverage ? 1 : 0);
        if (auto completed = complete->run(); !completed) return std::unexpected(completed.error());
        auto seen = writer_.prepare(
            "UPDATE roots SET availability='online', last_seen_at=?2 WHERE id=?1 AND availability<>'online'");
        if (!seen) return std::unexpected(seen.error());
        seen->bind(1, session.root_id).bind(2, now_unix_ms());
        if (auto updated = seen->run(); !updated) return std::unexpected(updated.error());
        if (writer_.changed_row_count() > 0) {  // the root came online: every sample's availability changed
            structure_changed = true;
            const std::int64_t root_parameter[] = {session.root_id};
            if (auto marked = detail::mark_samples_changed(writer_, kRootSamplesSql, root_parameter, false); !marked)
                return std::unexpected(marked.error());
        } else {
            auto touch = writer_.prepare("UPDATE roots SET last_seen_at=?2 WHERE id=?1");
            if (!touch) return std::unexpected(touch.error());
            touch->bind(1, session.root_id).bind(2, now_unix_ms());
            if (auto touched = touch->run(); !touched) return std::unexpected(touched.error());
        }
        if (auto blocked = detail::block_unreachable_jobs(writer_, session.root_id); !blocked)
            return std::unexpected(blocked.error());
        if (auto unblocked = detail::unblock_reachable_jobs(writer_, session.root_id); !unblocked)
            return std::unexpected(unblocked.error());
    }
    auto revision = detail::advance_revision(writer_, {.kind = "scan_finished",
                                                       .structure_changed = structure_changed,
                                                       .details = {{"root_id", session.root_id},
                                                                   {"cancelled", cancelled},
                                                                   {"marked_missing", outcome.marked_missing}}});
    if (!revision) return std::unexpected(revision.error());
    if (auto committed = commit_revision(transaction, *revision); !committed) return std::unexpected(committed.error());
    return outcome;
}

Result<std::map<std::string, Library::KnownStat>> Library::known_stats_in_directory(
    std::int64_t root_id, std::string_view relative_directory) {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto& connection = reader->connection();
    sqlite::ReadTransaction snapshot(connection);
    if (auto began = snapshot.begin(); !began) return std::unexpected(began.error());
    auto case_sensitive = root_case_sensitivity(connection, root_id);
    if (!case_sensitive) return std::unexpected(case_sensitive.error());
    auto statement = connection.prepare(
        relative_directory.empty()
            ? "SELECT path_key, byte_size, mtime_ns FROM locations WHERE root_id=?1 AND folder_id IS NULL "
              "AND state<>'retired'"
            : "SELECT location.path_key, location.byte_size, location.mtime_ns FROM locations location "
              "JOIN folders folder ON folder.id=location.folder_id WHERE location.root_id=?1 AND folder.root_id=?1 "
              "AND folder.path_key=?2 AND location.state<>'retired'");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, root_id);
    if (!relative_directory.empty()) statement->bind(2, mso::path_key(relative_directory, *case_sensitive));
    std::map<std::string, KnownStat> stats;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        stats.emplace(row.column_text(0), KnownStat{row.column_optional_int64(1), row.column_optional_int64(2)});
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return stats;
}

}  // namespace mso::catalog
