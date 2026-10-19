#pragma once
/// @file
/// Background supervisor of the durable analysis queue: leases one job at a time from the open
/// libraries (oldest run first, rotating libraries), runs it in the mso-worker child process,
/// renews the lease while the job makes progress, fences the result and publishes it through the
/// catalog writer. It only executes jobs of existing runs: runs are created by an explicit user
/// action (CLI `analyze`, the UI) or, in the desktop app's automatic mode, by the Application after
/// an import.
///
/// Hang protection: the worker client detects a task whose work counter stops moving
/// (WorkerOptions::task_stall_deadline) and ends it as a retryable WORKER_CRASH; on top of that every
/// job has a wall-clock deadline scaled by the sample's duration. When it passes, the task is
/// cancelled (the worker client terminates the process if the cancel is not acknowledged) and the
/// job is failed as retryable. The lease is not renewed while a cancel is pending. A worker that cannot be started is
/// retried with exponential backoff; a missing worker executable pauses the queue at once and is reported through
/// status(). A library that is being closed is abandoned: its running job is cancelled and returned to its queue,
/// so the supervisor never keeps a closed library (and its lock) alive.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "mso/catalog/library.hpp"

namespace mso::app {

/// Runs the durable analysis queue of the open libraries through mso-worker (see the file comment).
class AnalysisSupervisor {
public:
    /// Supplies the libraries to serve and resolves a sample's playable path; receives progress
    /// and errors. Called on the supervisor thread.
    struct Hooks {
        std::function<std::vector<std::shared_ptr<catalog::Library>>()> libraries;
        /// The playable file of a sample whose catalog detail was just read.
        std::function<Result<std::filesystem::path>(catalog::Library&, const catalog::SampleDetail& sample)>
            resolve_path;
        std::function<void(catalog::Library&, const std::string& worker_status)> on_progress;
        /// A catalog transition or worker step failed; `context` says which.
        std::function<void(catalog::Library*, const std::string& context, const Error& error)> on_error;
    };

    /// Time limits (defaults for production; tests shorten them).
    struct Timing {
        std::chrono::milliseconds idle_wait{750};               ///< poll interval with nothing to do
        std::chrono::milliseconds lease_duration{15'000};       ///< catalog lease length
        std::chrono::milliseconds lease_renew_interval{5'000};  ///< renewal cadence while a job runs
        std::chrono::milliseconds event_poll{200};              ///< worker event wait granularity
        std::chrono::milliseconds minimum_job_deadline{60'000};
        double deadline_per_audio_second = 4.0;  ///< extra wall-clock seconds per second of audio
        std::chrono::milliseconds maximum_job_deadline{30 * 60'000};
        std::chrono::milliseconds first_retry_delay{1'000};  ///< backoff after a failed start/submit
        std::chrono::milliseconds maximum_retry_delay{60'000};
        int failures_before_pause = 5;  ///< consecutive start/submit failures that pause the queue
    };

    /// Snapshot of the queue state.
    struct Status {
        bool paused = false;
        bool worker_unavailable = false;  ///< the last start attempt failed
        std::string detail;               ///< last worker status or failure
    };

    /// Starts the supervisor thread with the production time limits.
    AnalysisSupervisor(std::filesystem::path worker_executable, Hooks hooks);
    /// Starts the supervisor thread with explicit time limits.
    AnalysisSupervisor(std::filesystem::path worker_executable, Hooks hooks, Timing timing);
    /// Stops the loop, releases the job in flight and shuts the worker down.
    ~AnalysisSupervisor();
    AnalysisSupervisor(const AnalysisSupervisor&) = delete;
    AnalysisSupervisor& operator=(const AnalysisSupervisor&) = delete;

    /// Pauses or resumes leasing. Resuming clears a worker-unavailable state and ends a backoff wait.
    void set_paused(bool paused);
    /// True while leasing is paused.
    bool paused() const { return paused_.load(); }
    /// The current queue state.
    Status status() const;
    /// Wakes the idle loop immediately (after a new run was created or resumed). A backoff wait
    /// after worker start failures is not cut short by this; only resuming does that.
    void notify();
    /// Stops serving a library that is being closed: no new job of it is leased, and its running
    /// job is cancelled and returned to the queue within one event poll.
    void abandon_library(const uuids::uuid& library);
    /// Serves a library again (it was opened again after abandon_library()).
    void serve_library(const uuids::uuid& library);

private:
    void run(std::stop_token stop);
    /// One pass over the libraries; true when a job was processed.
    bool run_once(std::stop_token stop, std::size_t rotation);
    /// Runs one leased job to a published or recorded outcome.
    void process(catalog::Library& library, const catalog::Library::JobLease& lease, std::stop_token stop);
    /// Records a failed start/submit and waits out the backoff (or pauses after too many).
    void record_worker_failure(catalog::Library& library, const std::string& detail, bool permanent,
                               std::stop_token stop);
    /// Reports a failed catalog transition through Hooks::on_error.
    void report(catalog::Library* library, const std::string& context, const Result<void>& outcome) const;
    /// Reports a failed lease once per distinct message and library (the loop retries every pass).
    void report_lease_error(catalog::Library& library, const Error& error);
    /// True when `library` was abandoned (being closed).
    bool abandoned(const catalog::Library& library) const;
    void set_detail(std::string detail);

    std::filesystem::path worker_executable_;
    Hooks hooks_;
    Timing timing_;
    std::atomic<bool> paused_{false};
    std::atomic<bool> worker_unavailable_{false};
    std::atomic<int> consecutive_failures_{0};
    mutable std::mutex detail_mutex_;
    std::string detail_;
    std::mutex wake_mutex_;
    std::condition_variable_any wake_;
    bool wake_requested_ = false;   ///< notify(): ends an idle wait
    bool retry_requested_ = false;  ///< resume: ends a backoff wait too
    mutable std::mutex libraries_mutex_;
    std::set<uuids::uuid> abandoned_;  ///< libraries being closed
    /// Last lease error reported per library (cleared by a successful lease).
    std::map<uuids::uuid, std::string> last_lease_errors_;
    struct WorkerSlot;
    std::unique_ptr<WorkerSlot> worker_;
    std::jthread thread_;  ///< last member: started after everything above exists
};

}  // namespace mso::app
