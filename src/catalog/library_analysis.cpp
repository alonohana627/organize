// Explicit analysis runs: durable queue, leases, fencing, and publication of analysis facts
// (docs/05_AUDIO_INTELLIGENCE.md §1–2, docs/04_CONTRACTS.md §4–5).
#include <format>

#include <picosha2.h>

#include "mso/catalog/catalog_internal.hpp"
#include "mso/catalog/library.hpp"

namespace mso::catalog {

namespace {

constexpr int kMaxAttempts = 3;
constexpr std::int64_t kRetryBackoffMs[kMaxAttempts] = {1'000, 4'000, 16'000};

/// Bits of effective_metadata.analysis_flags; the search field `analyzed:` reads them.
std::int64_t stage_flag(std::string_view stage) {
    if (stage == "tempo") return 1;
    if (stage == "key") return 2;
    if (stage == "waveform") return 4;
    if (stage == "byte_hash" || stage == "pcm_hash") return 8;
    if (stage == "loudness") return 16;
    return 0;
}

/// Writes observations, hashes and artifacts of one verified result (caller holds the transaction).
Result<void> write_analysis_facts(sqlite::Connection& connection, const Library::AnalysisFacts& facts) {
    auto revision = connection.prepare("SELECT content_revision FROM samples WHERE id=?1");
    if (!revision) return std::unexpected(revision.error());
    revision->bind(1, facts.sample_id);
    auto has_row = revision->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::NotFound, "sample not found");
    if (revision->column_int64(0) != facts.content_revision)
        return fail(ErrorCode::InputChanged, "the sample changed while it was being analyzed");
    revision->reset();

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
        if (auto done = invalidate->run(); !done) return done;
        if (value.is_null()) return {};  // "no reliable estimate" also clears older analysis values
        insert->bind(1, facts.sample_id).bind(2, facts.content_revision).bind(3, field).bind(4, value.dump());
        insert->bind(5, algorithm).bind(6, confidence).bind(7, now_unix_ms());
        return insert->run();
    };

    std::int64_t flags = 0;
    if (facts.tempo_requested) {
        if (auto done = observe("bpm_milli", facts.bpm_milli ? nlohmann::json(*facts.bpm_milli) : nlohmann::json(),
                                facts.tempo_algorithm, facts.bpm_confidence);
            !done)
            return done;
        flags |= stage_flag("tempo");
    }
    if (facts.key_requested) {
        const nlohmann::json key = facts.key ? nlohmann::json{{"pitch_class", facts.key->pitch_class},
                                                              {"mode", std::string(to_string(facts.key->mode))}}
                                             : nlohmann::json();
        if (auto done = observe("key", key, facts.key_algorithm, facts.key_confidence); !done) return done;
        flags |= stage_flag("key");
    }
    if (facts.peak_dbfs && facts.rms_dbfs) {
        if (auto done = observe("peak_dbfs", *facts.peak_dbfs, "loudness-peak-rms-1", std::nullopt); !done) return done;
        if (auto done = observe("rms_dbfs", *facts.rms_dbfs, "loudness-peak-rms-1", std::nullopt); !done) return done;
        flags |= stage_flag("loudness");
    }
    if (facts.pcm_sha256) {
        if (auto done = observe("pcm_sha256", *facts.pcm_sha256, "sha256-pcm-s24-1", std::nullopt); !done) return done;
        flags |= stage_flag("pcm_hash");
    }
    if (facts.byte_sha256) {
        // SQLite's unhex() turns the worker's hex digest into the stored 32-byte BLOB.
        auto hash = connection.prepare(
            "UPDATE locations SET byte_hash=unhex(?2), hash_algorithm='sha256' WHERE sample_id=?1 AND state='present'");
        if (!hash) return std::unexpected(hash.error());
        hash->bind(1, facts.sample_id).bind(2, *facts.byte_sha256);
        if (auto done = hash->run(); !done) return done;
        flags |= stage_flag("byte_hash");
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
        artifact->bind(3, nlohmann::json{{"path", *facts.waveform_segment}}.dump());
        if (auto done = artifact->run(); !done) return done;
        flags |= stage_flag("waveform");
    }
    if (auto recomputed = detail::recompute_effective_musical_facts(connection, facts.sample_id); !recomputed)
        return recomputed;
    auto update =
        connection.prepare("UPDATE effective_metadata SET analysis_flags=analysis_flags | ?2 WHERE sample_id=?1");
    if (!update) return std::unexpected(update.error());
    update->bind(1, facts.sample_id).bind(2, flags);
    if (auto done = update->run(); !done) return done;
    return detail::advance_revision(
               connection, nlohmann::json{{"kind", "analysis"}, {"samples", nlohmann::json::array({facts.sample_id})}})
        .transform([](std::int64_t) {});
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

Result<std::int64_t> ensure_profile(sqlite::Connection& connection, const Library::AnalysisRunRequest& request) {
    const nlohmann::json config = {{"name", request.profile_name}, {"stages", request.stages}, {"version", 1}};
    const std::string digest = picosha2::hash256_hex_string(config.dump());
    auto find = connection.prepare("SELECT id FROM analysis_profiles WHERE digest=?1");
    if (!find) return std::unexpected(find.error());
    find->bind(1, digest);
    auto has_row = find->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (*has_row) return find->column_int64(0);
    find->reset();
    auto insert = connection.prepare(
        "INSERT INTO analysis_profiles(uuid,version,name,config_json,digest,created_at) VALUES(?1,1,?2,?3,?4,?5)");
    if (!insert) return std::unexpected(insert.error());
    insert->bind(1, uuids::to_string(new_uuid())).bind(2, request.profile_name).bind(3, config.dump()).bind(4, digest);
    insert->bind(5, now_unix_ms());
    if (auto inserted = insert->run(); !inserted) return std::unexpected(inserted.error());
    return connection.last_insert_rowid();
}

/// Fence check shared by completion and failure paths.
Result<void> verify_lease(sqlite::Connection& connection, const Library::JobLease& lease) {
    auto statement = connection.prepare(
        "SELECT j.state, j.lease_token, r.state, r.cancel_epoch FROM jobs j JOIN analysis_runs r ON r.id=j.run_id "
        "WHERE j.id=?1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, lease.job_id);
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::NotFound, "job not found");
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

}  // namespace

Result<void> Library::record_analysis(const AnalysisFacts& facts) {
    if (auto writable = ensure_writable(); !writable) return writable;
    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    if (auto written = write_analysis_facts(writer_, facts); !written) return written;
    return transaction.commit();
}

Result<Library::AnalysisRunStatus> Library::create_analysis_run(const AnalysisRunRequest& request) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    if (request.stages.empty()) return fail(ErrorCode::InvalidArgument, "an analysis profile needs at least one stage");
    if (request.sample_ids.empty()) return fail(ErrorCode::InvalidArgument, "no samples selected for analysis");
    std::int64_t required_flags = 0;
    for (const auto& stage : request.stages) {
        const auto flag = stage_flag(stage);
        if (!flag) return fail(ErrorCode::InvalidArgument, std::format("unknown analysis stage '{}'", stage));
        required_flags |= flag;
    }

    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    auto profile = ensure_profile(writer_, request);
    if (!profile) return std::unexpected(profile.error());
    auto revision = writer_.query_int64("SELECT catalog_revision FROM library_state WHERE singleton=1");
    if (!revision) return std::unexpected(revision.error());

    AnalysisRunStatus status;
    status.uuid = new_uuid();
    status.profile_name = request.profile_name;
    status.requested_at = now_unix_ms();
    auto run = writer_.prepare(
        "INSERT INTO analysis_runs(uuid,profile_id,state,requested_at,input_revision,selection_json,enumerated_count) "
        "VALUES(?1,?2,'preparing',?3,?4,?5,?6)");
    if (!run) return std::unexpected(run.error());
    run->bind(1, uuids::to_string(status.uuid)).bind(2, *profile).bind(3, status.requested_at).bind(4, *revision);
    run->bind(5, nlohmann::json{{"stages", request.stages}, {"force", request.force}}.dump());
    run->bind(6, static_cast<std::int64_t>(request.sample_ids.size()));
    if (auto inserted = run->run(); !inserted) return std::unexpected(inserted.error());
    status.id = writer_.last_insert_rowid();

    // Freeze the selection: one job per sample at its current content revision.
    auto sample = writer_.prepare(
        "SELECT s.content_revision, COALESCE(e.analysis_flags,0) FROM samples s LEFT JOIN effective_metadata e "
        "ON e.sample_id=s.id WHERE s.id=?1 AND s.deleted=0");
    auto job = writer_.prepare(
        "INSERT OR IGNORE INTO jobs(uuid,run_id,sample_id,content_revision,stage,state,checkpoint_json) "
        "VALUES(?1,?2,?3,?4,'bundle','queued',?5)");
    if (!sample) return std::unexpected(sample.error());
    if (!job) return std::unexpected(job.error());
    const std::string stages_json = nlohmann::json{{"stages", request.stages}}.dump();
    for (const auto sample_id : request.sample_ids) {
        sample->bind(1, sample_id);
        auto has_row = sample->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) {
            ++status.skipped;
            continue;
        }
        const auto content_revision = sample->column_int64(0);
        const bool current = (sample->column_int64(1) & required_flags) == required_flags;
        sample->reset();
        if (current && !request.force) {  // matching valid results already exist
            ++status.skipped;
            continue;
        }
        job->bind(1, uuids::to_string(new_uuid())).bind(2, status.id).bind(3, sample_id).bind(4, content_revision);
        job->bind(5, stages_json);
        if (auto inserted = job->run(); !inserted) return std::unexpected(inserted.error());
        ++status.queued;
    }
    status.state = status.queued > 0 ? "queued" : "completed";
    auto settle = writer_.prepare("UPDATE analysis_runs SET state=?2 WHERE id=?1");
    if (!settle) return std::unexpected(settle.error());
    settle->bind(1, status.id).bind(2, status.state);
    if (auto settled = settle->run(); !settled) return std::unexpected(settled.error());
    if (auto advanced = detail::advance_revision(
            writer_, nlohmann::json{{"kind", "analysis_run"}, {"run", uuids::to_string(status.uuid)}});
        !advanced)
        return std::unexpected(advanced.error());
    if (auto committed = transaction.commit(); !committed) return std::unexpected(committed.error());
    return status;
}

Result<void> Library::control_run(std::int64_t run_id, RunControl control) {
    if (auto writable = ensure_writable(); !writable) return writable;
    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    std::string sql;
    switch (control) {
        case RunControl::Pause:
            sql = "UPDATE analysis_runs SET state='paused' WHERE id=?1 AND state IN ('queued','running')";
            break;
        case RunControl::Resume:
            sql =
                "UPDATE analysis_runs SET state='queued' WHERE id=?1 AND state='paused';"
                "UPDATE jobs SET state='queued' WHERE run_id=?1 AND state IN ('interrupted','paused')";
            break;
        case RunControl::Cancel:
            // The epoch bump fences results of jobs that are still running in a worker.
            sql =
                "UPDATE analysis_runs SET state='cancelled', cancel_epoch=cancel_epoch+1 WHERE id=?1 "
                "AND state NOT IN ('completed','completed_with_errors','cancelled');"
                "UPDATE jobs SET state='cancelled', lease_token=NULL WHERE run_id=?1 "
                "AND state IN ('queued','blocked','paused','interrupted','running')";
            break;
    }
    // Execute each statement with the run id bound.
    std::size_t start = 0;
    while (start < sql.size()) {
        auto end = sql.find(';', start);
        if (end == std::string::npos) end = sql.size();
        auto statement = writer_.prepare(std::string_view(sql).substr(start, end - start));
        if (!statement) return std::unexpected(statement.error());
        statement->bind(1, run_id);
        if (auto ran = statement->run(); !ran) return ran;
        start = end + 1;
    }
    if (auto advanced =
            detail::advance_revision(writer_, nlohmann::json{{"kind", "analysis_control"}, {"run_id", run_id}});
        !advanced)
        return std::unexpected(advanced.error());
    return transaction.commit();
}

Result<std::vector<Library::AnalysisRunStatus>> Library::list_analysis_runs(std::size_t limit) {
    auto reader = acquire_reader();
    if (!reader) return std::unexpected(reader.error());
    auto statement = reader->connection().prepare(
        "SELECT r.id, r.uuid, p.name, r.state, r.requested_at, "
        "sum(j.state='queued'), sum(j.state='running'), sum(j.state='completed'), sum(j.state='failed'), "
        "sum(j.state IN ('stale')), sum(j.state='cancelled'), sum(j.state IN ('blocked','interrupted','paused')) "
        "FROM analysis_runs r JOIN analysis_profiles p ON p.id=r.profile_id LEFT JOIN jobs j ON j.run_id=r.id "
        "GROUP BY r.id ORDER BY r.id DESC LIMIT ?1");
    if (!statement) return std::unexpected(statement.error());
    statement->bind(1, static_cast<std::int64_t>(limit));
    std::vector<AnalysisRunStatus> runs;
    auto walked = detail::for_each_row(*statement, [&](sqlite::PreparedStatement& row) -> Result<void> {
        AnalysisRunStatus status;
        status.id = row.column_int64(0);
        status.uuid = uuids::uuid::from_string(row.column_text(1)).value_or(uuids::uuid());
        status.profile_name = row.column_text(2);
        status.state = row.column_text(3);
        status.requested_at = row.column_int64(4);
        status.queued = row.column_int64(5);
        status.running = row.column_int64(6);
        status.completed = row.column_int64(7);
        status.failed = row.column_int64(8);
        status.skipped = row.column_int64(9);
        status.cancelled = row.column_int64(10);
        status.blocked = row.column_int64(11);
        runs.push_back(std::move(status));
        return {};
    });
    if (!walked) return std::unexpected(walked.error());
    return runs;
}

Result<std::optional<Library::JobLease>> Library::lease_next_job(std::chrono::milliseconds lease_duration) {
    if (auto writable = ensure_writable(); !writable) return std::unexpected(writable.error());
    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return std::unexpected(began.error());
    const auto now = now_unix_ms();

    // Reclaim expired leases (a worker died without reporting).
    auto reclaim = writer_.prepare(
        "UPDATE jobs SET state='interrupted', lease_token=NULL, lease_expires_at=NULL "
        "WHERE state='running' AND lease_expires_at<?1");
    if (!reclaim) return std::unexpected(reclaim.error());
    reclaim->bind(1, now);
    if (auto reclaimed = reclaim->run(); !reclaimed) return std::unexpected(reclaimed.error());

    // Oldest run first; within it, the lowest job id whose backoff has elapsed. Jobs whose audio is
    // not reachable stay blocked until their root is online again.
    auto next = writer_.prepare(
        "SELECT j.id, j.run_id, j.uuid, j.sample_id, j.content_revision, j.checkpoint_json, j.attempt, r.cancel_epoch, "
        "EXISTS(SELECT 1 FROM locations l JOIN roots o ON o.id=l.root_id WHERE l.sample_id=j.sample_id "
        "AND l.state='present' AND o.availability<>'offline') FROM jobs j JOIN analysis_runs r ON r.id=j.run_id "
        "WHERE j.state IN ('queued','interrupted','blocked') AND r.state IN ('queued','running') "
        "AND (j.lease_expires_at IS NULL OR j.lease_expires_at<=?1) ORDER BY r.id, j.priority DESC, j.id LIMIT 32");
    if (!next) return std::unexpected(next.error());
    next->bind(1, now);
    std::optional<JobLease> lease;
    std::vector<std::int64_t> blocked;
    while (!lease) {
        auto has_row = next->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) break;
        if (next->column_int64(8) == 0) {
            blocked.push_back(next->column_int64(0));
            continue;
        }
        JobLease candidate;
        candidate.job_id = next->column_int64(0);
        candidate.run_id = next->column_int64(1);
        candidate.job_uuid = uuids::uuid::from_string(next->column_text(2)).value_or(uuids::uuid());
        candidate.sample_id = next->column_int64(3);
        candidate.content_revision = next->column_int64(4);
        const auto checkpoint = nlohmann::json::parse(next->column_optional_text(5).value_or("{}"), nullptr, false);
        if (checkpoint.is_object() && checkpoint.contains("stages"))
            candidate.stages = checkpoint["stages"].get<std::vector<std::string>>();
        candidate.attempt = next->column_int64(6) + 1;
        candidate.cancel_epoch = next->column_int64(7);
        candidate.lease_token = uuids::to_string(new_uuid());
        lease = std::move(candidate);
    }
    next->reset();
    for (const auto job_id : blocked)
        if (auto marked = set_job_state(writer_, job_id, "blocked", "SOURCE_OFFLINE", std::nullopt); !marked)
            return std::unexpected(marked.error());
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
    std::lock_guard lock(writer_mutex_);
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
    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    if (auto fenced = verify_lease(writer_, lease); !fenced) return fenced;
    if (auto written = write_analysis_facts(writer_, facts); !written) {
        if (written.error().code != ErrorCode::InputChanged) return written;
        writer_.rollback();
        // The sample changed while it was analyzed: discard the output, mark the job stale.
        sqlite::WriteTransaction stale(writer_);
        if (auto began_stale = stale.begin(); !began_stale) return began_stale;
        if (auto marked = set_job_state(writer_, lease.job_id, "stale", "INPUT_CHANGED", std::nullopt); !marked)
            return marked;
        if (auto settled = settle_run(writer_, lease.run_id); !settled) return settled;
        if (auto committed = stale.commit(); !committed) return committed;
        return written;
    }
    if (auto marked = set_job_state(writer_, lease.job_id, "completed", std::nullopt, std::nullopt); !marked)
        return marked;
    if (auto settled = settle_run(writer_, lease.run_id); !settled) return settled;
    return transaction.commit();
}

Result<void> Library::fail_job(const JobLease& lease, ErrorCode code, bool transient) {
    if (auto writable = ensure_writable(); !writable) return writable;
    std::lock_guard lock(writer_mutex_);
    sqlite::WriteTransaction transaction(writer_);
    if (auto began = transaction.begin(); !began) return began;
    if (auto fenced = verify_lease(writer_, lease); !fenced) return {};  // already cancelled: nothing to record
    const auto code_text = std::string(to_string(code));
    if (code == ErrorCode::InputChanged) {
        if (auto marked = set_job_state(writer_, lease.job_id, "stale", code_text, std::nullopt); !marked)
            return marked;
    } else if (code == ErrorCode::SourceOffline || code == ErrorCode::FileMissing) {
        if (auto marked = set_job_state(writer_, lease.job_id, "blocked", code_text, std::nullopt); !marked)
            return marked;
    } else if (transient && lease.attempt < kMaxAttempts) {
        const auto backoff = kRetryBackoffMs[std::clamp<std::int64_t>(lease.attempt - 1, 0, kMaxAttempts - 1)];
        if (auto marked = set_job_state(writer_, lease.job_id, "queued", code_text, now_unix_ms() + backoff); !marked)
            return marked;
    } else {
        if (auto marked = set_job_state(writer_, lease.job_id, "failed", code_text, std::nullopt); !marked)
            return marked;
    }
    if (auto settled = settle_run(writer_, lease.run_id); !settled) return settled;
    return transaction.commit();
}

Result<void> Library::release_job(const JobLease& lease) {
    if (auto writable = ensure_writable(); !writable) return writable;
    std::lock_guard lock(writer_mutex_);
    auto release = writer_.prepare(
        "UPDATE jobs SET state='queued', lease_token=NULL, lease_expires_at=NULL, attempt=max(attempt-1,0) "
        "WHERE id=?1 AND lease_token=?2 AND state='running'");
    if (!release) return std::unexpected(release.error());
    release->bind(1, lease.job_id).bind(2, lease.lease_token);
    return release->run();
}

}  // namespace mso::catalog
