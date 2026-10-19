// Analysis runs: durable queue, leases, fencing, and publication of analysis facts
// (docs/05_AUDIO_INTELLIGENCE.md §1–2, docs/04_CONTRACTS.md §4–5).
#include <algorithm>
#include <cmath>
#include <format>
#include <set>

#include <picosha2.h>

#include "mso/catalog/detail/catalog_internal.hpp"
#include "mso/catalog/library.hpp"
#include "mso/domain/analysis_flags.hpp"

namespace mso::catalog {

namespace {

constexpr std::int64_t kRetryBackoffMs[Library::kMaximumJobAttempts] = {1'000, 4'000, 16'000};
constexpr std::int64_t kMinimumMilliBpm = 20'000;
constexpr std::int64_t kMaximumMilliBpm = 400'000;
constexpr std::size_t kSha256HexDigits = 64;
constexpr std::size_t kMaximumProfileNameCodePoints = 128;

bool is_lowercase_or_uppercase_hex(std::string_view text) {
    return std::ranges::all_of(text, [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
    });
}

bool is_unit_interval(const std::optional<double>& value) {
    return !value || (std::isfinite(*value) && *value >= 0.0 && *value <= 1.0);
}

/// Range checks every fact before anything is written, so a bad worker result can never trip a
/// CHECK constraint mid-transaction and leave the job running until its lease expires.
Result<void> validate_facts(const Library::AnalysisFacts& facts) {
    auto invalid = [](std::string_view what) {
        return fail(ErrorCode::InvalidArgument, std::format("analysis result rejected: {}", what));
    };
    if (facts.bpm_milli && (*facts.bpm_milli < kMinimumMilliBpm || *facts.bpm_milli > kMaximumMilliBpm))
        return invalid("BPM outside 20–400");
    if (!is_unit_interval(facts.bpm_confidence) || !is_unit_interval(facts.key_confidence))
        return invalid("confidence outside 0–1");
    if (facts.key && (facts.key->pitch_class < 0 || facts.key->pitch_class > 11)) return invalid("pitch class");
    if ((facts.peak_dbfs && !std::isfinite(*facts.peak_dbfs)) || (facts.rms_dbfs && !std::isfinite(*facts.rms_dbfs)))
        return invalid("non-finite loudness");
    for (const auto* hash : {&facts.byte_sha256, &facts.pcm_sha256})
        if (*hash && ((*hash)->size() != kSha256HexDigits || !is_lowercase_or_uppercase_hex(**hash)))
            return invalid("SHA-256 must be 64 hex digits");
    if (facts.waveform_segment &&
        (!is_safe_relative_path(*facts.waveform_segment) || !is_valid_utf8(*facts.waveform_segment) ||
         !facts.waveform_segment->starts_with("derived/")))
        return invalid("waveform segment must be a safe path under derived/");
    if (!is_valid_utf8(facts.tempo_algorithm) || !is_valid_utf8(facts.key_algorithm))
        return invalid("algorithm names must be UTF-8");
    return {};
}

/// Validates stage names and returns the analysis_flags bits they require.
Result<std::int64_t> required_flags(const std::vector<std::string>& stages) {
    if (stages.empty()) return fail(ErrorCode::InvalidArgument, "an analysis profile needs at least one stage");
    std::int64_t flags = 0;
    for (const auto& stage : stages) {
        const auto flag = analysis_flag_for_stage(stage);
        if (!flag) return fail(ErrorCode::InvalidArgument, std::format("unknown analysis stage '{}'", stage));
        flags |= flag;
    }
    return flags;
}

/// The job's stage column: its sorted, de-duplicated stage names joined with '+'.
std::string stage_key(const std::vector<std::string>& stages) {
    const std::set<std::string> unique(stages.begin(), stages.end());
    std::string key;
    for (const auto& stage : unique) {
        if (!key.empty()) key += '+';
        key += stage;
    }
    return key;
}

/// Writes observations, hashes and artifacts of one verified result and advances the revision
/// (caller holds the transaction). Returns the new revision.
Result<std::int64_t> write_analysis_facts(sqlite::Connection& connection, const Library::AnalysisFacts& facts) {
    auto revision = connection.prepare("SELECT content_revision FROM samples WHERE id=?1");
    if (!revision) return std::unexpected(revision.error());
    revision->bind(1, facts.sample_id);
    auto has_row = revision->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::NotFound, "sample not found");
    const bool current = revision->column_int64(0) == facts.content_revision;
    revision->reset();
    if (!current) return fail(ErrorCode::InputChanged, "the sample changed while it was being analyzed");

    auto invalidate = connection.prepare(
        "UPDATE metadata_observations SET valid=0 WHERE sample_id=?1 AND source='analysis' AND field=?2");
    auto insert = connection.prepare(
        "INSERT INTO metadata_observations(sample_id,content_revision,field,value_json,source,algorithm_version,"
        "confidence,created_at) VALUES(?1,?2,?3,?4,'analysis',?5,?6,?7)");
    if (!invalidate) return std::unexpected(invalidate.error());
    if (!insert) return std::unexpected(insert.error());
    auto observe = [&](std::string_view field, const nlohmann::json& value, std::string_view algorithm,
                       std::optional<double> confidence) -> Result<void> {
        invalidate->bind(1, facts.sample_id).bind(2, field);
        if (auto done = invalidate->run(); !done) return std::unexpected(done.error());
        if (value.is_null()) return {};  // "no reliable estimate" also clears older analysis values
        insert->bind(1, facts.sample_id).bind(2, facts.content_revision).bind(3, field);
        insert->bind(4, detail::to_json_text(value)).bind(5, algorithm).bind(6, confidence).bind(7, now_unix_ms());
        return insert->run();
    };

    std::int64_t flags = 0;
    if (facts.tempo_requested) {
        if (auto done = observe("bpm_milli", facts.bpm_milli ? nlohmann::json(*facts.bpm_milli) : nlohmann::json(),
                                facts.tempo_algorithm, facts.bpm_confidence);
            !done)
            return std::unexpected(done.error());
        flags |= kAnalyzedTempo;
    }
    if (facts.key_requested) {
        const nlohmann::json key = facts.key ? nlohmann::json{{"pitch_class", facts.key->pitch_class},
                                                              {"mode", std::string(to_string(facts.key->mode))}}
                                             : nlohmann::json();
        if (auto done = observe("key", key, facts.key_algorithm, facts.key_confidence); !done)
            return std::unexpected(done.error());
        flags |= kAnalyzedKey;
    }
    if (facts.peak_dbfs && facts.rms_dbfs) {
        if (auto done = observe("peak_dbfs", *facts.peak_dbfs, "loudness-peak-rms-1", std::nullopt); !done)
            return std::unexpected(done.error());
        if (auto done = observe("rms_dbfs", *facts.rms_dbfs, "loudness-peak-rms-1", std::nullopt); !done)
            return std::unexpected(done.error());
        flags |= kAnalyzedLoudness;
    }
    if (facts.pcm_sha256) {
        if (auto done = observe("pcm_sha256", *facts.pcm_sha256, "sha256-pcm-s24-1", std::nullopt); !done)
            return std::unexpected(done.error());
        flags |= kAnalyzedHash;
    }
    if (facts.byte_sha256) {
        // SQLite's unhex() turns the worker's (validated) hex digest into the stored 32-byte BLOB.
        auto hash = connection.prepare(
            "UPDATE locations SET byte_hash=unhex(?2), hash_algorithm='sha256' WHERE sample_id=?1 AND state='present'");
        if (!hash) return std::unexpected(hash.error());
        hash->bind(1, facts.sample_id).bind(2, *facts.byte_sha256);
        if (auto done = hash->run(); !done) return std::unexpected(done.error());
        flags |= kAnalyzedHash;
    }
    if (facts.waveform_segment) {
        auto artifact = connection.prepare(
            "INSERT INTO "
            "artifacts(sample_id,content_revision,kind,algorithm_version,profile_digest,summary_json,state) "
            "VALUES(?1,?2,'waveform','waveform-minmaxrms-1','default',?3,'ready') "
            "ON CONFLICT(sample_id,content_revision,kind,algorithm_version,profile_digest) DO UPDATE SET "
            "summary_json=excluded.summary_json, state='ready'");
        if (!artifact) return std::unexpected(artifact.error());
        artifact->bind(1, facts.sample_id).bind(2, facts.content_revision);
        artifact->bind(3, detail::to_json_text(nlohmann::json{{"path", *facts.waveform_segment}}));
        if (auto done = artifact->run(); !done) return std::unexpected(done.error());
        flags |= kAnalyzedWaveform;
    }
    if (auto recomputed = detail::recompute_effective_musical_facts(connection, facts.sample_id); !recomputed)
        return std::unexpected(recomputed.error());
    auto update =
        connection.prepare("UPDATE effective_metadata SET analysis_flags=analysis_flags | ?2 WHERE sample_id=?1");
    if (!update) return std::unexpected(update.error());
    update->bind(1, facts.sample_id).bind(2, flags);
    if (auto done = update->run(); !done) return std::unexpected(done.error());
    if (auto marked = detail::mark_sample_changed(connection, facts.sample_id, /*search_text_changed=*/false); !marked)
        return std::unexpected(marked.error());
    return detail::advance_revision(connection, {.kind = "analysis"});
}

/// Marks a run completed once nothing is left to do.
Result<void> settle_run(sqlite::Connection& connection, std::int64_t run_id) {
    auto settle = connection.prepare(
        "UPDATE analysis_runs SET state=CASE WHEN EXISTS(SELECT 1 FROM jobs WHERE run_id=?1 AND state='failed') "
        "THEN 'completed_with_errors' ELSE 'completed' END WHERE id=?1 AND state IN ('queued','running') "
        "AND NOT EXISTS(SELECT 1 FROM jobs WHERE run_id=?1 AND state IN ('queued','running','blocked','interrupted'))");
    if (!settle) return std::unexpected(settle.error());
    settle->bind(1, run_id);
    return settle->run();
}

Result<std::int64_t> ensure_profile(sqlite::Connection& connection, std::string_view profile_name,
                                    const std::vector<std::string>& stages) {
    const nlohmann::json config = {{"name", profile_name}, {"stages", stages}, {"version", 1}};
    const std::string config_text = detail::to_json_text(config);
    const std::string digest = picosha2::hash256_hex_string(config_text);
    auto find = connection.prepare("SELECT id FROM analysis_profiles WHERE digest=?1");
    if (!find) return std::unexpected(find.error());
    find->bind(1, digest);
    auto has_row = find->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (*has_row) {
        const auto id = find->column_int64(0);
        find->reset();
        return id;
    }
    find->reset();
    auto insert = connection.prepare(
        "INSERT INTO analysis_profiles(uuid,version,name,config_json,digest,created_at) "
        "VALUES(new_uuid(),1,?1,?2,?3,?4)");
    if (!insert) return std::unexpected(insert.error());
    insert->bind(1, profile_name).bind(2, config_text).bind(3, digest).bind(4, now_unix_ms());
    if (auto inserted = insert->run(); !inserted) return std::unexpected(inserted.error());
    return connection.last_insert_rowid();
}

/// Fence check shared by completion and failure paths: CANCELLED when the lease is no longer held
/// or the run was cancelled; database errors are returned as they are.
Result<void> verify_lease(sqlite::Connection& connection, const Library::JobLease& lease) {
    auto statement = connection.prepare(
        "SELECT job.state, job.lease_token, run.state, run.cancel_epoch FROM jobs job "
        "JOIN analysis_runs run ON run.id=job.run_id WHERE job.id=?1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, lease.job_id);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::Cancelled, "the job no longer exists; result discarded");
    const bool lease_held = statement->column_text(0) == "running" && statement->column_text(1) == lease.lease_token;
    const std::string run_state = statement->column_text(2);
    const bool epoch_current = statement->column_int64(3) == lease.cancel_epoch;
    statement->reset();
    if (!lease_held || !epoch_current || run_state == "cancelled" || run_state == "cancelling")
        return fail(ErrorCode::Cancelled, "the job was cancelled or its lease expired; result discarded");
    return {};
}

Result<void> set_job_state(sqlite::Connection& connection, std::int64_t job_id, std::string_view state,
                           std::optional<std::string_view> error_code, std::optional<std::int64_t> not_before) {
    auto update = connection.prepare(
        "UPDATE jobs SET state=?2, error_code=?3, lease_token=NULL, lease_expires_at=?4 WHERE id=?1");
    if (!update) return std::unexpected(update.error());
    update->bind(1, job_id).bind(2, state);
    if (error_code)
        update->bind(3, *error_code);
    else
        update->bind_null(3);
    update->bind(4, not_before);
    return update->run();
}

/// Creates the run row (state 'preparing') and returns its status skeleton.
Result<Library::AnalysisRunStatus> insert_run(sqlite::Connection& connection, std::string_view profile_name,
                                              const std::vector<std::string>& stages, bool force,
                                              std::int64_t enumerated_count, const nlohmann::json& selection) {
    auto profile = ensure_profile(connection, profile_name, stages);
    if (!profile) return std::unexpected(profile.error());
    auto revision = connection.query_int64("SELECT catalog_revision FROM library_state WHERE singleton=1");
    if (!revision) return std::unexpected(revision.error());
    Library::AnalysisRunStatus status;
    status.uuid = new_uuid();
    status.profile_name = std::string(profile_name);
    status.requested_at = now_unix_ms();
    auto run = connection.prepare(
        "INSERT INTO analysis_runs(uuid,profile_id,state,requested_at,input_revision,selection_json,enumerated_count) "
        "VALUES(?1,?2,'preparing',?3,?4,?5,?6)");
    if (!run) return std::unexpected(run.error());
    nlohmann::json selection_json = selection;
    selection_json["stages"] = stages;
    selection_json["force"] = force;
    run->bind(1, uuids::to_string(status.uuid)).bind(2, *profile).bind(3, status.requested_at).bind(4, *revision);
    run->bind(5, detail::to_json_text(selection_json)).bind(6, enumerated_count);
    if (auto inserted = run->run(); !inserted) return std::unexpected(inserted.error());
    status.id = connection.last_insert_rowid();
    return status;
}

/// After the run's jobs were inserted (`inserted` of `enumerated` samples): blocks unreachable ones,
/// settles the run state and records the revision. Caller holds the transaction.
Result<std::int64_t> finish_run_creation(sqlite::Connection& connection, Library::AnalysisRunStatus& status,
                                         std::int64_t enumerated, std::int64_t inserted) {
    auto block = connection.prepare(std::format(
        "UPDATE jobs SET state='blocked', error_code='SOURCE_OFFLINE' WHERE run_id=?1 AND state='queued' AND NOT {}",
        detail::kJobSampleReachableSql));
    if (!block) return std::unexpected(block.error());
    block->bind(1, status.id);
    if (auto blocked = block->run(); !blocked) return std::unexpected(blocked.error());
    status.blocked = connection.changed_row_count();
    status.queued = inserted - status.blocked;
    status.skipped = enumerated - inserted;
    status.state = inserted > 0 ? "queued" : "completed";
    auto settle = connection.prepare("UPDATE analysis_runs SET state=?2 WHERE id=?1");
    if (!settle) return std::unexpected(settle.error());
    settle->bind(1, status.id).bind(2, status.state);
    if (auto settled = settle->run(); !settled) return std::unexpected(settled.error());
    return detail::advance_revision(connection,
                                    {.kind = "analysis_run", .details = {{"run", uuids::to_string(status.uuid)}}});
}

/// Shared argument checks of run creation.
Result<std::int64_t> validate_run_request(std::string_view profile_name, const std::vector<std::string>& stages) {
    if (trim(profile_name).empty() || !is_valid_utf8(profile_name) ||
        count_code_points(profile_name) > kMaximumProfileNameCodePoints)
        return fail(ErrorCode::InvalidArgument, "an analysis profile needs a name of 1–128 characters");
    return required_flags(stages);
}

}  // namespace

Result<void> Library::record_analysis(const AnalysisFacts& facts) {
    if (auto writable = ensure_writable(); !writable) return writable;
    if (auto valid = validate_facts(facts); !valid) return valid;
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    auto revision = write_analysis_facts(writer_, facts);
    if (!revision) return std::unexpected(revision.error());
    return commit_revision(transaction, *revision);
}

Result<Library::AnalysisRunStatus> Library::create_analysis_run(const AnalysisRunRequest& request) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    auto flags = validate_run_request(request.profile_name, request.stages);
    if (!flags) return std::unexpected(flags.error());
    std::vector<std::int64_t> sample_ids = request.sample_ids;
    std::ranges::sort(sample_ids);
    const auto [duplicates_begin, duplicates_end] = std::ranges::unique(sample_ids);
    sample_ids.erase(duplicates_begin, duplicates_end);
    if (sample_ids.empty()) return fail(ErrorCode::InvalidArgument, "no samples selected for analysis");

    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    const auto enumerated = static_cast<std::int64_t>(sample_ids.size());
    auto status = insert_run(writer_, request.profile_name, request.stages, request.force, enumerated,
                             nlohmann::json{{"kind", "samples"}});
    if (!status) return std::unexpected(status.error());

    // Freeze the selection set-based: one job per live sample at its current content revision.
    auto jobs = writer_.prepare(
        "INSERT INTO jobs(uuid,run_id,sample_id,content_revision,stage,state,checkpoint_json) "
        "SELECT new_uuid(), ?1, sample.id, sample.content_revision, ?2, 'queued', ?3 FROM samples sample "
        "LEFT JOIN effective_metadata effective ON effective.sample_id=sample.id "
        "WHERE sample.id IN (SELECT value FROM json_each(?4)) AND sample.deleted=0 "
        "AND (?5 OR (COALESCE(effective.analysis_flags,0) & ?6)<>?6) ORDER BY sample.id");
    if (!jobs) return std::unexpected(jobs.error());
    jobs->bind(1, status->id).bind(2, stage_key(request.stages));
    jobs->bind(3, detail::to_json_text(nlohmann::json{{"stages", request.stages}}));
    jobs->bind(4, detail::to_json_text(nlohmann::json(sample_ids))).bind(5, request.force ? 1 : 0).bind(6, *flags);
    if (auto inserted = jobs->run(); !inserted) return std::unexpected(inserted.error());
    const auto inserted = static_cast<std::int64_t>(writer_.changed_row_count());

    auto revision = finish_run_creation(writer_, *status, enumerated, inserted);
    if (!revision) return std::unexpected(revision.error());
    if (auto committed = commit_revision(transaction, *revision); !committed) return std::unexpected(committed.error());
    return status;
}

Result<Library::AnalysisRunStatus> Library::create_analysis_run_for_root(std::int64_t root_id,
                                                                         std::string_view profile_name,
                                                                         const std::vector<std::string>& stages,
                                                                         bool force) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    auto flags = validate_run_request(profile_name, stages);
    if (!flags) return std::unexpected(flags.error());

    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    auto root = writer_.prepare("SELECT 1 FROM roots WHERE id=?1 AND deleted=0");
    if (!root) return std::unexpected(root.error());
    root->bind(1, root_id);
    auto root_exists = root->next_row();
    if (!root_exists) return std::unexpected(root_exists.error());
    root->reset();
    if (!*root_exists) return fail(ErrorCode::NotFound, "no such root");

    constexpr std::string_view kRootLiveSamples =
        "SELECT location.sample_id FROM locations location WHERE location.root_id=?1 AND location.state<>'retired'";
    auto count = writer_.prepare(std::format(
        "SELECT count(*) FROM samples sample WHERE sample.deleted=0 AND sample.id IN ({})", kRootLiveSamples));
    if (!count) return std::unexpected(count.error());
    count->bind(1, root_id);
    auto counted = count->next_row();
    if (!counted) return std::unexpected(counted.error());
    const std::int64_t enumerated = *counted ? count->column_int64(0) : 0;
    count->reset();

    auto status = insert_run(writer_, profile_name, stages, force, enumerated,
                             nlohmann::json{{"kind", "root"}, {"root_id", root_id}});
    if (!status) return std::unexpected(status.error());
    auto jobs = writer_.prepare(
        std::format("INSERT INTO jobs(uuid,run_id,sample_id,content_revision,stage,state,checkpoint_json) "
                    "SELECT new_uuid(), ?2, sample.id, sample.content_revision, ?3, 'queued', ?4 FROM samples sample "
                    "LEFT JOIN effective_metadata effective ON effective.sample_id=sample.id "
                    "WHERE sample.id IN ({}) AND sample.deleted=0 "
                    "AND (?5 OR (COALESCE(effective.analysis_flags,0) & ?6)<>?6) ORDER BY sample.id",
                    kRootLiveSamples));
    if (!jobs) return std::unexpected(jobs.error());
    jobs->bind(1, root_id).bind(2, status->id).bind(3, stage_key(stages));
    jobs->bind(4, detail::to_json_text(nlohmann::json{{"stages", stages}})).bind(5, force ? 1 : 0).bind(6, *flags);
    if (auto inserted = jobs->run(); !inserted) return std::unexpected(inserted.error());
    const auto inserted = static_cast<std::int64_t>(writer_.changed_row_count());

    auto revision = finish_run_creation(writer_, *status, enumerated, inserted);
    if (!revision) return std::unexpected(revision.error());
    if (auto committed = commit_revision(transaction, *revision); !committed) return std::unexpected(committed.error());
    return status;
}

Result<void> Library::control_run(std::int64_t run_id, RunControl control) {
    if (auto writable = ensure_writable(); !writable) return writable;
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    auto exists = writer_.prepare("SELECT 1 FROM analysis_runs WHERE id=?1");
    if (!exists) return std::unexpected(exists.error());
    exists->bind(1, run_id);
    auto found = exists->next_row();
    if (!found) return std::unexpected(found.error());
    exists->reset();
    if (!*found) return fail(ErrorCode::NotFound, "no such analysis run");

    std::vector<std::string_view> statements;
    switch (control) {
        case RunControl::Pause:
            statements = {"UPDATE analysis_runs SET state='paused' WHERE id=?1 AND state IN ('queued','running')"};
            break;
        case RunControl::Resume:
            statements = {"UPDATE analysis_runs SET state='queued' WHERE id=?1 AND state='paused'",
                          "UPDATE jobs SET state='queued' WHERE run_id=?1 AND state IN ('interrupted','paused')"};
            break;
        case RunControl::Cancel:
            // The epoch bump fences results of jobs that are still running in a worker.
            statements = {
                "UPDATE analysis_runs SET state='cancelled', cancel_epoch=cancel_epoch+1 WHERE id=?1 "
                "AND state NOT IN ('completed','completed_with_errors','cancelled')",
                "UPDATE jobs SET state='cancelled', lease_token=NULL WHERE run_id=?1 "
                "AND state IN ('queued','blocked','paused','interrupted','running')"};
            break;
    }
    int changed = 0;
    for (const auto sql : statements) {
        auto statement = writer_.prepare(sql);
        if (!statement) return std::unexpected(statement.error());
        statement->bind(1, run_id);
        if (auto ran = statement->run(); !ran) return ran;
        changed += writer_.changed_row_count();
    }
    if (changed == 0) return {};  // already in the requested state: nothing to commit
    if (control == RunControl::Resume) {
        auto block =
            writer_.prepare(std::format("UPDATE jobs SET state='blocked', error_code='SOURCE_OFFLINE' WHERE run_id=?1 "
                                        "AND state='queued' AND NOT {}",
                                        detail::kJobSampleReachableSql));
        if (!block) return std::unexpected(block.error());
        block->bind(1, run_id);
        if (auto blocked = block->run(); !blocked) return blocked;
    }
    auto revision = detail::advance_revision(writer_, {.kind = "analysis_control", .details = {{"run_id", run_id}}});
    if (!revision) return std::unexpected(revision.error());
    return commit_revision(transaction, *revision);
}

Result<std::vector<Library::AnalysisRunStatus>> Library::list_analysis_runs(std::size_t limit) {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto statement = reader->connection().prepare(
        "SELECT run.id, run.uuid, profile.name, run.state, run.requested_at, "
        "COALESCE(sum(job.state='queued'),0), COALESCE(sum(job.state='running'),0), "
        "COALESCE(sum(job.state='completed'),0), COALESCE(sum(job.state='failed'),0), "
        "COALESCE(sum(job.state='stale'),0), COALESCE(sum(job.state='cancelled'),0), "
        "COALESCE(sum(job.state IN ('blocked','interrupted','paused')),0), max(run.enumerated_count-count(job.id),0) "
        "FROM analysis_runs run JOIN analysis_profiles profile ON profile.id=run.profile_id "
        "LEFT JOIN jobs job ON job.run_id=run.id GROUP BY run.id ORDER BY run.id DESC LIMIT ?1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, static_cast<std::int64_t>(std::min<std::size_t>(limit, 100'000)));
    std::vector<AnalysisRunStatus> runs;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        AnalysisRunStatus status;
        status.id = row.column_int64(0);
        auto uuid = detail::parse_stored_uuid(row.column_text(1), "analysis run");
        if (!uuid) return std::unexpected(uuid.error());
        status.uuid = *uuid;
        status.profile_name = row.column_text(2);
        status.state = row.column_text(3);
        status.requested_at = row.column_int64(4);
        status.queued = row.column_int64(5);
        status.running = row.column_int64(6);
        status.completed = row.column_int64(7);
        status.failed = row.column_int64(8);
        status.stale = row.column_int64(9);
        status.cancelled = row.column_int64(10);
        status.blocked = row.column_int64(11);
        status.skipped = row.column_int64(12);
        runs.push_back(std::move(status));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return runs;
}

Result<std::optional<Library::JobLease>> Library::lease_next_job(std::chrono::milliseconds lease_duration) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    const auto now = now_unix_ms();

    // Reclaim expired leases (a worker died without reporting). A job whose lease expired on its
    // last allowed attempt fails instead of looping forever.
    auto reclaim = writer_.prepare(
        "UPDATE jobs SET state=CASE WHEN attempt>=?2 THEN 'failed' ELSE 'interrupted' END, "
        "error_code=CASE WHEN attempt>=?2 THEN 'WORKER_CRASH' ELSE error_code END, lease_token=NULL, "
        "lease_expires_at=NULL WHERE state='running' AND lease_expires_at<?1 RETURNING run_id");
    if (!reclaim) return std::unexpected(reclaim.error());
    reclaim->bind(1, now).bind(2, kMaximumJobAttempts);
    std::set<std::int64_t> reclaimed_runs;
    auto walked = detail::for_each_row(*reclaim, [&](sqlite::PreparedStatement& row) -> Result<void> {
        reclaimed_runs.insert(row.column_int64(0));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    for (const auto run_id : reclaimed_runs)
        if (auto settled = settle_run(writer_, run_id); !settled) return std::unexpected(settled.error());

    // Oldest run first; within it, priority then the lowest job id whose backoff has elapsed. Only
    // jobs whose audio is reachable qualify, so an offline drive never holds up other jobs.
    auto next = writer_.prepare(std::format(
        "SELECT jobs.id, jobs.run_id, jobs.uuid, jobs.sample_id, jobs.content_revision, jobs.checkpoint_json, "
        "jobs.attempt, run.cancel_epoch, jobs.stage FROM jobs JOIN analysis_runs run ON run.id=jobs.run_id "
        "WHERE jobs.state IN ('queued','interrupted') AND run.state IN ('queued','running') "
        "AND (jobs.lease_expires_at IS NULL OR jobs.lease_expires_at<=?1) AND {} "
        "ORDER BY run.id, jobs.priority DESC, jobs.id LIMIT 1",
        detail::kJobSampleReachableSql));
    if (!next) return std::unexpected(next.error());
    next->bind(1, now);
    auto has_row = next->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    std::optional<JobLease> lease;
    if (*has_row) {
        JobLease candidate;
        candidate.job_id = next->column_int64(0);
        candidate.run_id = next->column_int64(1);
        const std::string job_uuid = next->column_text(2);
        candidate.sample_id = next->column_int64(3);
        candidate.content_revision = next->column_int64(4);
        const auto checkpoint = detail::parse_json(next->column_optional_text(5).value_or("{}"));
        candidate.attempt = next->column_int64(6) + 1;
        candidate.cancel_epoch = next->column_int64(7);
        const std::string stage_column = next->column_text(8);
        next->reset();
        auto parsed_uuid = detail::parse_stored_uuid(job_uuid, "job");
        if (!parsed_uuid) return std::unexpected(parsed_uuid.error());
        candidate.job_uuid = *parsed_uuid;
        auto stages = detail::json_string_array_member(checkpoint, "stages");
        if (!stages || stages->empty())
            return fail(ErrorCode::DbCorrupt,
                        std::format("job {} has no stage list ({})", candidate.job_id, stage_column));
        candidate.stages = std::move(*stages);
        candidate.lease_token = uuids::to_string(new_uuid());
        lease = std::move(candidate);
    } else {
        next->reset();
    }
    if (lease) {
        auto take = writer_.prepare(
            "UPDATE jobs SET state='running', lease_token=?2, lease_expires_at=?3, attempt=?4, error_code=NULL WHERE "
            "id=?1");
        if (!take) return std::unexpected(take.error());
        take->bind(1, lease->job_id)
            .bind(2, lease->lease_token)
            .bind(3, now + lease_duration.count())
            .bind(4, lease->attempt);
        if (auto taken = take->run(); !taken) return std::unexpected(taken.error());
        auto run = writer_.prepare("UPDATE analysis_runs SET state='running' WHERE id=?1 AND state='queued'");
        if (!run) return std::unexpected(run.error());
        run->bind(1, lease->run_id);
        if (auto started = run->run(); !started) return std::unexpected(started.error());
    }
    if (auto committed = transaction.commit(); !committed) return std::unexpected(committed.error());
    return lease;
}

Result<void> Library::renew_lease(const JobLease& lease, std::chrono::milliseconds lease_duration) {
    if (auto writable = ensure_writable(); !writable) return writable;
    const std::lock_guard lock(writer_mutex_);
    auto renew =
        writer_.prepare("UPDATE jobs SET lease_expires_at=?3 WHERE id=?1 AND lease_token=?2 AND state='running'");
    if (!renew) return std::unexpected(renew.error());
    renew->bind(1, lease.job_id).bind(2, lease.lease_token).bind(3, now_unix_ms() + lease_duration.count());
    if (auto renewed = renew->run(); !renewed) return renewed;
    if (writer_.changed_row_count() == 0) return fail(ErrorCode::Cancelled, "the lease is no longer held");
    return {};
}

Result<void> Library::complete_job(const JobLease& lease, const AnalysisFacts& facts) {
    if (auto writable = ensure_writable(); !writable) return writable;
    const auto valid = validate_facts(facts);
    const bool matches_lease = facts.sample_id == lease.sample_id && facts.content_revision == lease.content_revision;
    const std::lock_guard lock(writer_mutex_);
    {
        sqlite::WriteTransaction transaction(writer_);
        if (auto began = transaction.begin(); !began) return began;
        if (auto fenced = verify_lease(writer_, lease); !fenced) return fenced;
        if (valid && matches_lease) {
            auto revision = write_analysis_facts(writer_, facts);
            if (revision) {
                if (auto marked = set_job_state(writer_, lease.job_id, "completed", std::nullopt, std::nullopt);
                    !marked)
                    return marked;
                if (auto settled = settle_run(writer_, lease.run_id); !settled) return settled;
                return commit_revision(transaction, *revision);
            }
            if (revision.error().code != ErrorCode::InputChanged)
                return std::unexpected(revision.error());  // stays leased; reclaim retries within the cap
        }
    }  // the failed attempt rolls back here

    // Bad facts fail the job permanently; changed (or mismatched) input discards the output as stale.
    const Error outcome = !valid ? valid.error()
                                 : Error{ErrorCode::InputChanged, false,
                                         matches_lease ? "the sample changed while it was being analyzed"
                                                       : "the result does not belong to the leased sample revision",
                                         facts.sample_id, std::nullopt};
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    if (auto fenced = verify_lease(writer_, lease); !fenced) return fenced;
    if (auto marked =
            set_job_state(writer_, lease.job_id, valid ? "stale" : "failed", to_string(outcome.code), std::nullopt);
        !marked)
        return marked;
    if (auto settled = settle_run(writer_, lease.run_id); !settled) return settled;
    if (auto committed = transaction.commit(); !committed) return committed;
    return std::unexpected(outcome);
}

Result<void> Library::fail_job(const JobLease& lease, ErrorCode code, bool transient) {
    if (auto writable = ensure_writable(); !writable) return writable;
    const std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    if (auto fenced = verify_lease(writer_, lease); !fenced) {
        if (fenced.error().code == ErrorCode::Cancelled) return {};  // no longer ours: nothing to record
        return fenced;
    }
    const auto code_text = std::string(to_string(code));
    Result<void> marked;
    if (code == ErrorCode::InputChanged) {
        marked = set_job_state(writer_, lease.job_id, "stale", code_text, std::nullopt);
    } else if (code == ErrorCode::SourceOffline || code == ErrorCode::FileMissing) {
        // Unblocked again when its root comes online or a scan finds the file (finish_scan).
        marked = set_job_state(writer_, lease.job_id, "blocked", code_text, std::nullopt);
    } else if (transient && lease.attempt < kMaximumJobAttempts) {
        const auto backoff = kRetryBackoffMs[std::clamp<std::int64_t>(lease.attempt - 1, 0, kMaximumJobAttempts - 1)];
        marked = set_job_state(writer_, lease.job_id, "queued", code_text, now_unix_ms() + backoff);
    } else {
        marked = set_job_state(writer_, lease.job_id, "failed", code_text, std::nullopt);
    }
    if (!marked) return marked;
    if (auto settled = settle_run(writer_, lease.run_id); !settled) return settled;
    return transaction.commit();
}

Result<void> Library::release_job(const JobLease& lease) {
    if (auto writable = ensure_writable(); !writable) return writable;
    const std::lock_guard lock(writer_mutex_);
    auto release = writer_.prepare(
        "UPDATE jobs SET state='queued', lease_token=NULL, lease_expires_at=NULL, attempt=max(attempt-1,0) "
        "WHERE id=?1 AND lease_token=?2 AND state='running'");
    if (!release) return std::unexpected(release.error());
    release->bind(1, lease.job_id).bind(2, lease.lease_token);
    return release->run();
}

}  // namespace mso::catalog
