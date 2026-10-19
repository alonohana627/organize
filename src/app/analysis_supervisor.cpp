#include "mso/app/analysis_supervisor.hpp"

#include <picosha2.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>

#include "mso/analysis/analysis.hpp"
#include "mso/worker/worker_process.hpp"

namespace mso::app {

namespace {

using Clock = std::chrono::steady_clock;

/// Grace after a deadline cancel before the worker is torn down regardless (the worker client
/// normally terminates it itself after its cancel_grace).
constexpr auto kCancelBackstop = std::chrono::seconds(10);

/// The known stages among stored stage names (unknown names are skipped).
std::vector<analysis::Stage> parse_stages(const std::vector<std::string>& names) {
    std::vector<analysis::Stage> stages;
    for (const auto& name : names)
        if (auto stage = analysis::parse_stage(name)) stages.push_back(*stage);
    return stages;
}

/// True when `wanted` is among `stages`.
bool has_stage(const std::vector<analysis::Stage>& stages, analysis::Stage wanted) {
    return std::ranges::find(stages, wanted) != stages.end();
}

/// Digest of what the task computes (stage names and their algorithm versions); echoed by the
/// worker in every outcome fence, so results of a different profile can never be confused.
std::string profile_digest(const std::vector<std::string>& stage_names, const std::vector<analysis::Stage>& stages) {
    std::string description;
    for (std::size_t index = 0; index < stages.size(); ++index)
        description += std::format("{}={};", index < stage_names.size() ? stage_names[index] : std::string(),
                                   analysis::algorithm_version(stages[index]));
    return picosha2::hash256_hex_string(description);
}

/// Validates a worker-written waveform segment before it is referenced by the catalog. The size is
/// checked before anything is read, so a wrong or runaway file is never loaded into memory.
Result<void> verify_waveform_segment(const std::filesystem::path& file, const worker::WaveformSegment& segment) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(file, size_error);
    if (size_error) return fail(ErrorCode::FileMissing, "waveform segment missing");
    if (size != segment.byte_length) return fail(ErrorCode::CorruptAudio, "waveform segment has the wrong size");
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail(ErrorCode::FileMissing, "waveform segment missing");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() != segment.byte_length)
        return fail(ErrorCode::CorruptAudio, "waveform segment has the wrong size");
    if (picosha2::hash256_hex_string(bytes.begin(), bytes.end()) != segment.sha256)
        return fail(ErrorCode::CorruptAudio, "waveform segment checksum mismatch");
    auto decoded = analysis::decode_waveform(bytes);
    if (!decoded) return std::unexpected(decoded.error());
    return {};
}

/// Removes a waveform segment the catalog will not reference (failed verification or publish).
void discard_segment(const std::filesystem::path& file) {
    std::error_code ignored;  // best effort: an orphan file is harmless and rewritten on retry
    std::filesystem::remove(file, ignored);
}

}  // namespace

std::chrono::milliseconds AnalysisSupervisor::job_deadline(const Timing& timing,
                                                           std::optional<std::int64_t> duration_us) {
    const double audio_seconds = duration_us ? std::max(0.0, static_cast<double>(*duration_us) / 1e6) : 0.0;
    const auto scaled = std::chrono::milliseconds(
        static_cast<std::int64_t>(std::min(audio_seconds * timing.deadline_per_audio_second * 1000.0,
                                           static_cast<double>(timing.maximum_job_deadline.count()))));
    return std::clamp(timing.minimum_job_deadline + scaled, timing.minimum_job_deadline,
                      std::max(timing.minimum_job_deadline, timing.maximum_job_deadline));
}

std::chrono::milliseconds AnalysisSupervisor::retry_delay(const Timing& timing, int failures) {
    auto delay = timing.first_retry_delay;
    for (int doubling = 1; doubling < failures && delay < timing.maximum_retry_delay; ++doubling)
        delay *= 2;
    return std::min(delay, timing.maximum_retry_delay);
}

/// The lazily started worker process, restarted after a crash.
struct AnalysisSupervisor::WorkerSlot {
    std::unique_ptr<worker::WorkerProcess> process;
};

AnalysisSupervisor::AnalysisSupervisor(std::filesystem::path worker_executable, Hooks hooks)
    : AnalysisSupervisor(std::move(worker_executable), std::move(hooks), Timing{}) {}

AnalysisSupervisor::AnalysisSupervisor(std::filesystem::path worker_executable, Hooks hooks, Timing timing)
    : AnalysisSupervisor(worker::WorkerOptions{.executable = std::move(worker_executable)}, std::move(hooks), timing) {}

AnalysisSupervisor::AnalysisSupervisor(worker::WorkerOptions worker_options, Hooks hooks, Timing timing)
    : worker_options_(std::move(worker_options)),
      hooks_(std::move(hooks)),
      timing_(timing),
      worker_(std::make_unique<WorkerSlot>()) {
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

AnalysisSupervisor::~AnalysisSupervisor() {
    thread_.request_stop();
    notify();
    if (thread_.joinable()) thread_.join();
    if (worker_->process) worker_->process->shutdown();
}

void AnalysisSupervisor::set_paused(bool paused) {
    if (!paused) {
        worker_unavailable_.store(false);
        consecutive_failures_.store(0);
    }
    paused_.store(paused);
    {
        std::lock_guard lock(wake_mutex_);
        wake_requested_ = true;
        if (!paused) retry_requested_ = true;
    }
    wake_.notify_all();
}

void AnalysisSupervisor::abandon_library(const uuids::uuid& library) {
    std::lock_guard lock(libraries_mutex_);
    abandoned_.insert(library);
}

void AnalysisSupervisor::serve_library(const uuids::uuid& library) {
    {
        std::lock_guard lock(libraries_mutex_);
        abandoned_.erase(library);
    }
    notify();
}

bool AnalysisSupervisor::abandoned(const catalog::Library& library) const {
    std::lock_guard lock(libraries_mutex_);
    return abandoned_.contains(library.uuid());
}

void AnalysisSupervisor::report_lease_error(catalog::Library& library, const Error& error) {
    {
        std::lock_guard lock(libraries_mutex_);
        auto& last = last_lease_errors_[library.uuid()];
        if (last == error.message()) return;
        last = error.message();
    }
    if (hooks_.on_error) hooks_.on_error(&library, "leasing an analysis job", error);
}

AnalysisSupervisor::Status AnalysisSupervisor::status() const {
    std::lock_guard lock(detail_mutex_);
    return Status{paused_.load(), worker_unavailable_.load(), detail_};
}

void AnalysisSupervisor::set_detail(std::string detail) {
    std::lock_guard lock(detail_mutex_);
    detail_ = std::move(detail);
}

void AnalysisSupervisor::notify() {
    {
        std::lock_guard lock(wake_mutex_);
        wake_requested_ = true;
    }
    wake_.notify_all();
}

void AnalysisSupervisor::report(catalog::Library* library, const std::string& context,
                                const Result<void>& outcome) const {
    if (!outcome && hooks_.on_error) hooks_.on_error(library, context, outcome.error());
}

void AnalysisSupervisor::run(std::stop_token stop) {
    std::size_t rotation = 0;
    while (!stop.stop_requested()) {
        bool worked = false;
        // Exception barrier: a throw in one pass is reported and the loop keeps serving.
        try {
            worked = run_once(stop, rotation++);
        } catch (const std::exception& exception) {
            if (hooks_.on_error)
                hooks_.on_error(nullptr, "analysis supervisor",
                                Error{ErrorCode::Internal, true, exception.what(), std::nullopt, std::nullopt});
        }
        if (worked) continue;
        std::unique_lock lock(wake_mutex_);
        wake_.wait_for(lock, stop, timing_.idle_wait, [&] { return wake_requested_; });
        wake_requested_ = false;
    }
}

bool AnalysisSupervisor::run_once(std::stop_token stop, std::size_t rotation) {
    if (paused_.load()) return false;
    auto libraries = hooks_.libraries();
    for (std::size_t offset = 0; offset < libraries.size() && !stop.stop_requested(); ++offset) {
        auto& library = *libraries[(rotation + offset) % libraries.size()];
        if (library.read_only() || abandoned(library)) continue;
        auto lease = library.lease_next_job(timing_.lease_duration);
        if (!lease) {
            report_lease_error(library, lease.error());
            continue;
        }
        {
            std::lock_guard lock(libraries_mutex_);
            last_lease_errors_.erase(library.uuid());
        }
        if (!*lease) continue;
        process(library, **lease, stop);
        if (hooks_.on_progress) hooks_.on_progress(library, paused_.load() ? "paused" : "idle");
        return true;
    }
    return false;
}

void AnalysisSupervisor::record_worker_failure(catalog::Library& library, const std::string& detail, bool permanent,
                                               std::stop_token stop) {
    worker_unavailable_.store(true);
    set_detail("worker unavailable: " + detail);
    if (hooks_.on_progress) hooks_.on_progress(library, "worker unavailable: " + detail);
    const int failures = consecutive_failures_.fetch_add(1) + 1;
    if (permanent || failures >= timing_.failures_before_pause) {
        paused_.store(true);  // do not spin; resuming retries
        return;
    }
    // Exponential backoff before the next attempt (ended early by shutdown or by resuming; a plain
    // notify() for new work does not end it, or repeated new runs would defeat the backoff).
    const auto delay = retry_delay(timing_, failures);
    std::unique_lock lock(wake_mutex_);
    retry_requested_ = false;
    wake_.wait_for(lock, stop, delay, [&] { return retry_requested_; });
    retry_requested_ = false;
}

void AnalysisSupervisor::process(catalog::Library& library, const catalog::Library::JobLease& lease,
                                 std::stop_token stop) {
    const auto stages = parse_stages(lease.stages);
    auto detail = library.sample_detail(lease.sample_id);
    if (!detail) {
        report(&library, "recording a job whose sample cannot be read",
               library.fail_job(lease, detail.error().code, false));
        return;
    }
    auto path = hooks_.resolve_path(library, *detail);
    if (!path) {
        report(&library, "recording an unreachable sample",
               library.fail_job(lease, path.error().code, path.error().retryable));
        return;
    }
    const auto lease_id = uuids::uuid::from_string(lease.lease_token);
    if (!lease_id) {
        report(&library, "recording a malformed lease", library.fail_job(lease, ErrorCode::Internal, false));
        return;
    }
    if (!worker_->process || !worker_->process->is_running()) {
        std::error_code missing_check;
        const auto executable =
            worker_options_.executable.empty() ? worker::default_worker_executable() : worker_options_.executable;
        const bool executable_missing = !std::filesystem::is_regular_file(executable, missing_check);
        auto started = worker::WorkerProcess::start(worker_options_);
        if (!started) {
            report(&library, "returning a job after the worker failed to start", library.release_job(lease));
            record_worker_failure(library, started.error().message(), executable_missing, stop);
            return;
        }
        worker_->process = std::move(*started);
    }

    const auto relative_segment =
        std::format("derived/waveforms/{}-r{}.mswf", uuids::to_string(detail->uuid), lease.content_revision);
    const auto segment_file = library.directory() / relative_segment;
    worker::AnalyzeTask task;
    task.job_id = lease.job_uuid;
    task.lease_id = *lease_id;
    task.cancel_epoch = lease.cancel_epoch;
    task.content_revision = lease.content_revision;
    task.profile_digest = profile_digest(lease.stages, stages);
    task.file_path = path_to_utf8(*path);
    task.stages = stages;
    if (has_stage(stages, analysis::Stage::Waveform)) task.waveform_output_path = path_to_utf8(segment_file);

    if (hooks_.on_progress) hooks_.on_progress(library, "analyzing " + detail->display_name);
    set_detail("analyzing " + detail->display_name);
    if (auto submitted = worker_->process->submit(task); !submitted) {
        report(&library, "returning a job the worker refused", library.release_job(lease));
        worker_->process.reset();
        record_worker_failure(library, submitted.error().message(), false, stop);
        return;
    }
    consecutive_failures_.store(0);
    worker_unavailable_.store(false);

    const auto started_at = Clock::now();
    const auto deadline = started_at + job_deadline(timing_, detail->duration_us);
    auto last_renewal = started_at;
    std::optional<Clock::time_point> cancelled_at;
    bool timed_out = false;
    bool renewing = true;
    auto cancel_task = [&] {
        if (cancelled_at) return;
        cancelled_at = Clock::now();
        // NOT_FOUND only means the outcome is already on its way; nothing else to do.
        (void)worker_->process->cancel(task.job_id, task.cancel_epoch);
    };
    while (true) {
        if (stop.stop_requested()) {
            cancel_task();
            report(&library, "returning a job at shutdown", library.release_job(lease));
            return;
        }
        if (abandoned(library)) {
            // The library is closing: give the job back at once so nothing keeps it (and its lock)
            // alive, and stop the worker so no task of it is still in flight for the next job.
            cancel_task();
            report(&library, "returning a job of a closing library", library.release_job(lease));
            worker_->process.reset();
            return;
        }
        const auto now = Clock::now();
        // Stalls (no decoder progress) are detected by the worker client itself (task_stall_deadline)
        // and end as a retryable WORKER_CRASH; this is the overall wall-clock budget on top.
        if (!cancelled_at && now >= deadline) {
            timed_out = true;
            cancel_task();
        }
        if (cancelled_at && now - *cancelled_at >= kCancelBackstop) {
            // The worker never answered the cancel: tear it down and fail the job as retryable.
            worker_->process.reset();
            report(&library, "recording a hung analysis job", library.fail_job(lease, ErrorCode::WorkerCrash, true));
            return;
        }
        const auto activity = worker_->process->task_activity();
        const bool cancelling = cancelled_at.has_value() || (activity && activity->cancelling);
        if (renewing && !cancelling && now - last_renewal >= timing_.lease_renew_interval) {
            last_renewal = now;
            if (auto renewed = library.renew_lease(lease, timing_.lease_duration); !renewed) {
                // The run was cancelled or the lease lost: stop the work, any result is discarded.
                renewing = false;
                cancel_task();
            }
        }
        auto event = worker_->process->next_event(timing_.event_poll);
        if (!event) continue;
        if (std::holds_alternative<worker::Progress>(*event)) continue;
        if (const auto* exited = std::get_if<worker::WorkerExited>(&*event)) {
            const Error error = exited->failure.value_or(Error{ErrorCode::WorkerCrash, true, "worker exited", {}, {}});
            report(&library, "recording a worker crash",
                   library.fail_job(lease, timed_out ? ErrorCode::WorkerCrash : error.code, true));
            worker_->process.reset();
            return;
        }
        if (const auto* failed = std::get_if<worker::TaskFailed>(&*event)) {
            if (failed->fence != worker::TaskFence::of(task)) continue;
            if (timed_out)
                report(&library, "recording a timed-out analysis job",
                       library.fail_job(lease, ErrorCode::WorkerCrash, true));
            else
                report(&library, "recording a failed analysis job",
                       library.fail_job(lease, failed->code, failed->retryable));
            return;
        }
        if (const auto* result = std::get_if<worker::TaskResult>(&*event)) {
            if (result->fence != worker::TaskFence::of(task)) continue;  // late result of an older task
            const auto& report_data = result->report;
            catalog::Library::AnalysisFacts facts;
            facts.sample_id = lease.sample_id;
            facts.content_revision = lease.content_revision;
            facts.tempo_requested = has_stage(stages, analysis::Stage::Tempo);
            facts.key_requested = has_stage(stages, analysis::Stage::Key);
            facts.tempo_algorithm = analysis::AlgorithmVersions::kTempo;
            facts.key_algorithm = analysis::AlgorithmVersions::kKey;
            facts.loudness_algorithm = analysis::AlgorithmVersions::kLoudness;
            facts.pcm_hash_algorithm = analysis::AlgorithmVersions::kPcmHash;
            facts.waveform_algorithm = analysis::AlgorithmVersions::kWaveform;
            if (report_data.tempo) {
                facts.bpm_milli = report_data.tempo->milli_bpm;
                facts.bpm_confidence = report_data.tempo->confidence;
            }
            if (report_data.key) {
                facts.key = report_data.key->key;
                facts.key_confidence = report_data.key->confidence;
            }
            if (report_data.loudness) {
                facts.peak_dbfs = report_data.loudness->peak_dbfs;
                facts.rms_dbfs = report_data.loudness->rms_dbfs;
            }
            facts.byte_sha256 = report_data.hashes.byte_sha256;
            facts.pcm_sha256 = report_data.hashes.pcm_sha256;
            if (result->waveform_segment) {
                if (auto verified = verify_waveform_segment(segment_file, *result->waveform_segment); verified) {
                    facts.waveform_segment = relative_segment;
                } else {
                    discard_segment(segment_file);
                    report(&library, "verifying a waveform segment", verified);
                }
            }
            if (!report_data.evidence.unchanged()) {
                if (facts.waveform_segment) discard_segment(segment_file);
                report(&library, "recording a file that changed during analysis",
                       library.fail_job(lease, ErrorCode::InputChanged, false));
                return;
            }
            // Fenced again inside the catalog; a refused publish leaves no referenced segment.
            auto completed = library.complete_job(lease, facts);
            if (!completed && facts.waveform_segment) discard_segment(segment_file);
            if (!completed && completed.error().code != ErrorCode::Cancelled &&
                completed.error().code != ErrorCode::InputChanged)
                report(&library, "publishing an analysis result", completed);
            return;
        }
    }
}

}  // namespace mso::app
