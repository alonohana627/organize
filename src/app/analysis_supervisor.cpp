#include "mso/app/analysis_supervisor.hpp"

#include <picosha2.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>

#include "mso/analysis/analysis.hpp"
#include "mso/analysis/worker_process.hpp"

namespace mso::app {

namespace {

constexpr auto kIdleWait = std::chrono::milliseconds(750);
constexpr auto kLeaseDuration = std::chrono::milliseconds(15'000);
constexpr auto kLeaseRenewInterval = std::chrono::milliseconds(5'000);
constexpr auto kEventPoll = std::chrono::milliseconds(200);

std::vector<analysis::Stage> parse_stages(const std::vector<std::string>& names) {
    std::vector<analysis::Stage> stages;
    for (const auto& name : names)
        if (auto stage = analysis::parse_stage(name)) stages.push_back(*stage);
    return stages;
}

bool has_stage(const std::vector<analysis::Stage>& stages, analysis::Stage wanted) {
    return std::ranges::find(stages, wanted) != stages.end();
}

/// Validates a worker-written waveform segment before it is referenced by the catalog.
Result<void> verify_waveform_segment(const std::filesystem::path& file, const worker::WaveformSegment& segment) {
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

}  // namespace

/// The lazily started worker process, restarted after a crash.
struct AnalysisSupervisor::WorkerSlot {
    std::unique_ptr<worker::WorkerProcess> process;
    std::string last_error;
};

AnalysisSupervisor::AnalysisSupervisor(std::filesystem::path worker_executable, Hooks hooks)
    : worker_executable_(std::move(worker_executable)),
      hooks_(std::move(hooks)),
      worker_(std::make_unique<WorkerSlot>()) {
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

AnalysisSupervisor::~AnalysisSupervisor() {
    thread_.request_stop();
    notify();
    if (thread_.joinable()) thread_.join();
    if (worker_->process) worker_->process->shutdown();
}

void AnalysisSupervisor::notify() {
    {
        std::lock_guard lock(wake_mutex_);
        wake_requested_ = true;
    }
    wake_.notify_all();
}

void AnalysisSupervisor::run(std::stop_token stop) {
    std::size_t rotation = 0;
    while (!stop.stop_requested()) {
        bool worked = false;
        if (!paused_.load()) {
            auto libraries = hooks_.libraries();
            for (std::size_t offset = 0; offset < libraries.size() && !worked && !stop.stop_requested(); ++offset) {
                auto& library = *libraries[(rotation + offset) % libraries.size()];
                if (library.read_only()) continue;
                auto lease = library.lease_next_job(kLeaseDuration);
                if (!lease || !*lease) continue;
                process(library, **lease, stop);
                if (hooks_.on_progress) hooks_.on_progress(library, "idle");
                worked = true;
            }
            ++rotation;
        }
        if (worked) continue;
        std::unique_lock lock(wake_mutex_);
        wake_.wait_for(lock, stop, kIdleWait, [&] { return wake_requested_; });
        wake_requested_ = false;
    }
}

void AnalysisSupervisor::process(catalog::Library& library, const catalog::Library::JobLease& lease,
                                 std::stop_token stop) {
    const auto stages = parse_stages(lease.stages);
    auto path = hooks_.resolve_path(library, lease.sample_id);
    if (!path) {
        (void)library.fail_job(lease, path.error().code, path.error().retryable);
        return;
    }
    if (!worker_->process || !worker_->process->is_running()) {
        auto started = worker::WorkerProcess::start({.executable = worker_executable_});
        if (!started) {
            worker_->last_error = started.error().message();
            if (hooks_.on_progress) hooks_.on_progress(library, "worker unavailable: " + worker_->last_error);
            (void)library.release_job(lease);
            paused_.store(true);  // do not spin; the user resumes once the problem is fixed
            return;
        }
        worker_->process = std::move(*started);
    }

    auto detail = library.sample_detail(lease.sample_id);
    if (!detail) {
        (void)library.fail_job(lease, detail.error().code, false);
        return;
    }
    const auto relative_segment =
        std::format("derived/waveforms/{}-r{}.mswf", uuids::to_string(detail->uuid), lease.content_revision);
    worker::AnalyzeTask task;
    task.job_id = lease.job_uuid;
    task.lease_id = uuids::uuid::from_string(lease.lease_token).value_or(new_uuid());
    task.cancel_epoch = lease.cancel_epoch;
    task.content_revision = lease.content_revision;
    task.profile_digest = "default";
    task.file_path = path_to_utf8(*path);
    task.stages = stages;
    if (has_stage(stages, analysis::Stage::Waveform))
        task.waveform_output_path = path_to_utf8(library.directory() / relative_segment);

    if (hooks_.on_progress) hooks_.on_progress(library, "analyzing " + detail->display_name);
    if (auto submitted = worker_->process->submit(task); !submitted) {
        (void)library.release_job(lease);
        worker_->process.reset();
        return;
    }

    auto last_renewal = std::chrono::steady_clock::now();
    while (true) {
        if (stop.stop_requested()) {
            (void)worker_->process->cancel(task.job_id, task.cancel_epoch);
            (void)library.release_job(lease);
            return;
        }
        if (std::chrono::steady_clock::now() - last_renewal >= kLeaseRenewInterval) {
            last_renewal = std::chrono::steady_clock::now();
            if (!library.renew_lease(lease, kLeaseDuration)) {
                // The run was cancelled or the lease lost: stop the work, discard any result.
                (void)worker_->process->cancel(task.job_id, task.cancel_epoch);
            }
        }
        auto event = worker_->process->next_event(kEventPoll);
        if (!event) continue;
        if (std::holds_alternative<worker::Progress>(*event)) continue;
        if (auto* exited = std::get_if<worker::WorkerExited>(&*event)) {
            const Error error = exited->failure.value_or(Error{ErrorCode::WorkerCrash, true, "worker exited", {}, {}});
            (void)library.fail_job(lease, error.code, true);
            worker_->process.reset();
            return;
        }
        if (auto* failed = std::get_if<worker::TaskFailed>(&*event)) {
            if (failed->fence != worker::TaskFence::of(task)) continue;
            (void)library.fail_job(lease, failed->code, failed->retryable);
            return;
        }
        if (auto* result = std::get_if<worker::TaskResult>(&*event)) {
            if (result->fence != worker::TaskFence::of(task)) continue;  // late result of an older task
            const auto& report = result->report;
            catalog::Library::AnalysisFacts facts;
            facts.sample_id = lease.sample_id;
            facts.content_revision = lease.content_revision;
            facts.tempo_requested = has_stage(stages, analysis::Stage::Tempo);
            facts.key_requested = has_stage(stages, analysis::Stage::Key);
            facts.tempo_algorithm = analysis::AlgorithmVersions::kTempo;
            facts.key_algorithm = analysis::AlgorithmVersions::kKey;
            if (report.tempo) {
                facts.bpm_milli = report.tempo->milli_bpm;
                facts.bpm_confidence = report.tempo->confidence;
            }
            if (report.key) {
                facts.key = report.key->key;
                facts.key_confidence = report.key->confidence;
            }
            if (report.loudness) {
                facts.peak_dbfs = report.loudness->peak_dbfs;
                facts.rms_dbfs = report.loudness->rms_dbfs;
            }
            facts.byte_sha256 = report.hashes.byte_sha256;
            facts.pcm_sha256 = report.hashes.pcm_sha256;
            if (result->waveform_segment) {
                const auto file = library.directory() / relative_segment;
                if (verify_waveform_segment(file, *result->waveform_segment)) facts.waveform_segment = relative_segment;
            }
            if (!report.evidence.unchanged()) {
                (void)library.fail_job(lease, ErrorCode::InputChanged, false);
                return;
            }
            (void)library.complete_job(lease, facts);  // fenced again inside the catalog
            return;
        }
    }
}

}  // namespace mso::app
